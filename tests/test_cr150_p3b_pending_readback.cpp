// P40: real command populations, ordered streaming/page readback, cancellation
// and a nested read. The owning projection is an explicit non-flat oracle.
// RED against 6cfcff0696: visit_source_pending/source_pending_page are absent.
#include <pineforge/source/pine_strategy_host.hpp>
#include "cr150_p3b_allocations.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
int failures = 0;
int checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (false)
constexpr double na = std::numeric_limits<double>::quiet_NaN();

bool same(double a, double b) { return a == b || (std::isnan(a) && std::isnan(b)); }

void equal(const source::PineStrategyHost::FixtureIntentRow& a,
           const source::PineStrategyHost::FixtureIntentRow& b) {
    CHECK(a.id == b.id);
    CHECK(a.type == b.type);
    CHECK(a.from_entry == b.from_entry);
    CHECK(a.is_long == b.is_long);
    CHECK(same(a.qty, b.qty));
    CHECK(same(a.qty_percent, b.qty_percent));
    CHECK(a.created_bar == b.created_bar);
    CHECK(a.created_seq == b.created_seq);
    CHECK(a.incarnation == b.incarnation);
    CHECK(a.over_pyramiding_cap_at_placement == b.over_pyramiding_cap_at_placement);
    CHECK(same(a.frozen_default_qty, b.frozen_default_qty));
    CHECK(same(a.default_stop_placement_qty, b.default_stop_placement_qty));
    CHECK(same(a.default_stop_sizing_price, b.default_stop_sizing_price));
    CHECK(same(a.frozen_market_own_units, b.frozen_market_own_units));
    CHECK(same(a.frozen_market_transaction_units, b.frozen_market_transaction_units));
    std::vector<admission::Field> left, right;
    admission::reflect(a.market_admission, "draft", [&](const auto& f) { left.push_back(f); });
    admission::reflect(b.market_admission, "draft", [&](const auto& f) { right.push_back(f); });
    CHECK(left.size() == right.size());
    for (std::size_t i = 0; i < left.size() && i < right.size(); ++i) {
        CHECK(left[i].path == right[i].path);
        CHECK(left[i].value.index() == right[i].value.index());
        if (const auto* x = std::get_if<double>(&left[i].value))
            CHECK(same(*x, std::get<double>(right[i].value)));
        else CHECK(left[i].value == right[i].value);
    }
}

class Host final : public source::PineStrategyHost {
public:
    explicit Host(int width) : width_(width) {
        source::PineStrategyConfig c;
        c.initial_capital = 1e9;
        c.pyramiding = width + 1;
        c.margin_long = c.margin_short = 0;
        configure_pine_strategy(c);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        for (int i = 0; i < width_; ++i)
            strategy_entry("pending-" + std::to_string(i), true, na, 1000.0 + i, i + 1.0);

        // Sample the FIRST readback, before the owning oracle or any page can
        // warm a hidden cache. The already-populated book is outside the
        // sample; both retained and transient readback allocations are counted.
        std::size_t cursor = 0;
        cr150_p3b_allocations::begin();
        const bool completed = visit_source_pending([&](const auto& row) {
            CHECK(row.id == "pending-" + std::to_string(cursor));
            CHECK(row.qty == cursor + 1.0);
            ++cursor;
            return true;
        });
        cr150_p3b_allocations::end();
        CHECK(completed && cursor == static_cast<std::size_t>(width_));
        CHECK(cr150_p3b_allocations::live == 0);
        CHECK(cr150_p3b_allocations::peak <= 8192);

        const auto oracle = source_pending_view();
        CHECK(oracle.size() == static_cast<std::size_t>(width_));
        cursor = 0;
        CHECK(visit_source_pending([&](const auto& row) {
            CHECK(cursor < oracle.size());
            CHECK(row.id == "pending-" + std::to_string(cursor));
            CHECK(row.qty == cursor + 1.0);
            if (cursor < oracle.size()) equal(row, oracle[cursor]);
            ++cursor;
            return true;
        }));
        CHECK(cursor == oracle.size());

        // Page widths do not divide either population. No row may be repeated
        // or lost at a boundary, and an exact-end read must return no rows.
        std::array<FixtureIntentRow, 3> page;
        for (std::size_t start = 0; start < oracle.size(); start += page.size()) {
            const auto count = source_pending_page(start, page.data(), page.size());
            CHECK(count == std::min(page.size(), oracle.size() - start));
            for (std::size_t i = 0; i < count; ++i) equal(page[i], oracle[start + i]);
        }
        CHECK(source_pending_page(oracle.size(), page.data(), page.size()) == 0);
        CHECK(source_pending_page(std::numeric_limits<std::size_t>::max(), page.data(), page.size()) == 0);
        CHECK(source_pending_page(0, nullptr, 0) == 0);

        cursor = 0;
        CHECK(!visit_source_pending([&](const auto&) { return ++cursor < 7; }));
        CHECK(cursor == 7);
        cursor = 0;
        CHECK(visit_source_pending([&](const auto& outer) {
            const auto saved_id = outer.id;
            const auto saved_quantity = outer.qty;
            FixtureIntentRow inner;
            CHECK(source_pending_page(oracle.size() - 1, &inner, 1) == 1);
            equal(inner, oracle.back());
            CHECK(outer.id == saved_id && outer.qty == saved_quantity);
            ++cursor;
            return true;
        }));
        CHECK(cursor == oracle.size());
        observed = cursor;
    }
    std::size_t observed = 0;
private:
    int width_;
};
}

int main() {
    for (const int width : {37, 257}) {
        Host host(width);
        const Bar bars[] = {{100, 101, 99, 100, 1, 60000},
                            {100, 101, 99, 100, 1, 120000}};
        host.run(bars, 2, "1", "1");
        CHECK(host.last_error().empty());
        CHECK(host.observed == static_cast<std::size_t>(width));
        CHECK(host.trade_count() == 0);
    }
    std::printf("P40 ordered pending readback: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
