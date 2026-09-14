/*
 * R4-D L0 literal legacy-route oracle for the no-fill-event-budget rule.
 * Captured at ab9714beccb62b796c122cf68986ec9e7dbf4a67.
 */

#include <cmath>
#include <cstdio>
#include <string>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

class DenseRestingBook final : public source::PineStrategyHost {
public:
    DenseRestingBook() {
        calc_on_order_fills_ = true;
        initial_capital_ = 100000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 100;
        commission_value_ = 0.0;
        margin_call_enabled_ = false;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ != 0 || submitted_) return;
        submitted_ = true;
        for (int i = 0; i != 65; ++i)
            strategy_entry("E" + std::to_string(i), true, 99.0 - 0.1 * i);
    }
    int lots() const { return static_cast<int>(pyramid_entries_.size()); }
    double signed_units() const { return signed_position_size(); }
private:
    bool submitted_ = false;
};
}  // namespace

int main() {
    DenseRestingBook book;
    const Bar bars[] = {
        {100, 101, 99, 100, 1, 1000},
        {100, 120, 90, 100, 1, 2000},
    };
    book.run(bars, 2);
    CHECK(book.last_error().empty());
    CHECK(book.trade_count() == 0);
    CHECK(book.lots() == 65);            // literal: every resting limit fills, not 64.
    CHECK(std::abs(book.signed_units() - 65.0) < 1e-12);
    std::printf("R4-D >64-fill oracle: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
