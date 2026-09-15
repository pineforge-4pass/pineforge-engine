// CHECK-parity native-route port of test_source_fifo_endpoints.cpp.
//
// The former fixture fabricated source lots and called the deleted pending
// owner directly.  This twin drives the same FIFO/ANY facts through source
// commands on a PineStrategyHost and observes only trades, position and the
// read-only pending projection.
#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(value) do {                                                       \
    ++checks;                                                                   \
    if (!(value)) {                                                             \
        ++failures;                                                             \
        std::printf("FAIL %s:%d %s\\n", __FILE__, __LINE__, #value);          \
    }                                                                           \
} while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Bar bar(double o, double h, double l, double c, std::int64_t t) {
    return {o, h, l, c, 1.0, t};
}

class FifoHost final : public source::PineStrategyHost {
public:
    enum class Case { AnyReentry, ReplacementGrowth, DeferredPercent, NoTarget, FifoId };

    explicit FifoHost(Case which) : which_(which) {
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.commission_value = 0.0;
        config.pyramiding = 10;
        config.close_entries_rule_any = which != Case::FifoId;
        configure_pine_strategy(config);
        margin_call_enabled_ = false;
    }

    void on_source_bar(const Bar&) override {
        switch (which_) {
        case Case::AnyReentry:
            if (pine_bar_index() == 0) strategy_entry("E", true, kNaN, kNaN, 1.0);
            if (pine_bar_index() == 1) strategy_entry("E", true, kNaN, kNaN, 2.0);
            if (pine_bar_index() == 2)
                strategy_exit("ANY", "E", 105.0, kNaN, kNaN, kNaN, kNaN, 100.0);
            break;
        case Case::ReplacementGrowth:
            if (pine_bar_index() == 0) {
                strategy_entry("E", true, 95.0, kNaN, 1.0);
                strategy_exit("X", "E", 105.0, kNaN, kNaN, kNaN, kNaN, 100.0);
            }
            if (pine_bar_index() == 1) strategy_entry("E", true, 95.0, kNaN, 2.0);
            break;
        case Case::DeferredPercent:
            if (pine_bar_index() == 0) {
                strategy_entry("E", true, 95.0, kNaN, 4.0);
                strategy_exit("HALF", "E", 105.0, kNaN, kNaN, kNaN, kNaN, 50.0);
            }
            break;
        case Case::NoTarget:
            if (pine_bar_index() == 0) {
                strategy_entry("NEVER", true, 50.0, kNaN, 1.0);
                strategy_exit("WAIT", "NEVER", 105.0, kNaN, kNaN, kNaN, kNaN, 100.0);
                strategy_close("NEVER");
            }
            break;
        case Case::FifoId:
            if (pine_bar_index() == 0) strategy_entry("A", true, kNaN, kNaN, 1.0);
            if (pine_bar_index() == 1) strategy_entry("B", true, kNaN, kNaN, 2.0);
            if (pine_bar_index() == 2) strategy_close("A", "FIFO");
            break;
        }
    }

    double position() const { return live_position_size(); }
    int pending() const { return pending_order_count(); }

private:
    Case which_;
};

void any_reentry_closes_every_same_id_opening() {
    FifoHost host(FifoHost::Case::AnyReentry);
    const Bar tape[] = {
        bar(100, 100, 100, 100, 1000), bar(100, 100, 94, 96, 2000),
        bar(96, 106, 96, 105, 3000), bar(105, 105, 105, 105, 4000),
    };
    host.run(tape, 4);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    if (host.trade_count() == 2) {
        const Trade& first = host.get_trade(0);
        const Trade& second = host.get_trade(1);
        CHECK(first.entry_id == "E");
        CHECK(second.entry_id == "E");
        CHECK(first.qty == 1.0);
        CHECK(second.qty == 2.0);
        CHECK(first.entry_price == 100.0);
        CHECK(second.entry_price == 96.0);
        CHECK(first.exit_id == "ANY");
        CHECK(second.exit_id == "ANY");
    }
    CHECK(std::abs(host.position()) < 1e-12);
}

void replacement_growth_rebinds_at_the_candidate() {
    FifoHost host(FifoHost::Case::ReplacementGrowth);
    const Bar tape[] = {
        bar(100, 100, 100, 100, 1000), bar(100, 100, 100, 100, 2000),
        bar(100, 100, 94, 96, 3000), bar(96, 106, 96, 105, 4000),
    };
    host.run(tape, 4);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const Trade& row = host.get_trade(0);
        CHECK(row.entry_id == "E");
        CHECK(row.exit_id == "X");
        CHECK(row.qty == 2.0);
        CHECK(row.entry_price == 95.0);
        CHECK(row.exit_price == 105.0);
        CHECK(row.entry_time == 3000);
        CHECK(row.exit_time == 4000);
    }
    CHECK(std::abs(host.position()) < 1e-12);
}

void deferred_percent_resolves_the_live_cohort() {
    FifoHost host(FifoHost::Case::DeferredPercent);
    const Bar tape[] = {
        bar(100, 100, 100, 100, 1000), bar(100, 100, 94, 96, 2000),
        bar(96, 106, 96, 105, 3000), bar(105, 105, 105, 105, 4000),
    };
    host.run(tape, 4);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const Trade& row = host.get_trade(0);
        CHECK(row.entry_id == "E");
        CHECK(row.exit_id == "HALF");
        CHECK(row.qty == 2.0);
        CHECK(row.entry_price == 95.0);
        CHECK(row.exit_price == 105.0);
        CHECK(row.entry_time == 2000);
        CHECK(row.exit_time == 3000);
    }
    CHECK(std::abs(host.position() - 2.0) < 1e-12);
    CHECK(host.pending() == 0);
}

void never_opened_any_target_stays_live_while_close_drops() {
    FifoHost host(FifoHost::Case::NoTarget);
    const Bar tape[] = {
        bar(100, 100, 100, 100, 1000), bar(100, 100, 100, 100, 2000),
        bar(100, 100, 100, 100, 3000),
    };
    host.run(tape, 3);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 0);
    CHECK(std::abs(host.position()) < 1e-12);
    CHECK(host.pending() == 2);
    CHECK(host.live_position_size() == 0.0);
}

void fifo_close_keeps_an_unrelated_later_opening() {
    FifoHost host(FifoHost::Case::FifoId);
    const Bar tape[] = {
        bar(100, 100, 100, 100, 1000), bar(100, 100, 100, 100, 2000),
        bar(100, 100, 100, 100, 3000), bar(100, 100, 100, 100, 4000),
        bar(100, 100, 100, 100, 5000),
    };
    host.run(tape, 5);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const Trade& row = host.get_trade(0);
        CHECK(row.entry_id == "A");
        CHECK(row.exit_id == "__close__A");
        CHECK(row.exit_comment == "FIFO");
        CHECK(row.qty == 1.0);
        CHECK(row.entry_price == 100.0);
        CHECK(row.exit_price == 100.0);
        CHECK(row.entry_time == 2000);
        CHECK(row.exit_time == 4000);
    }
    CHECK(std::abs(host.position() - 2.0) < 1e-12);
    CHECK(host.pending() == 0);
    CHECK(host.live_position_size() == 2.0);
}

}  // namespace

int main() {
    any_reentry_closes_every_same_id_opening();
    replacement_growth_rebinds_at_the_candidate();
    deferred_percent_resolves_the_live_cohort();
    never_opened_any_target_stays_live_while_close_drops();
    fifo_close_keeps_an_unrelated_later_opening();
    std::printf("%s source FIFO endpoints: %d checks, %d failures\\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures == 0 ? 0 : 1;
}
