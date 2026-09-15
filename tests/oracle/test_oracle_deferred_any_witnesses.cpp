/*
 * R4-D L0 literal legacy-route oracle — deferred-ANY pin witnesses 1–4.
 * Captured at ab9714beccb62b796c122cf68986ec9e7dbf4a67.  This deliberately
 * uses only the current source::PineStrategyHost command surface; all values
 * below were observed on the LegacyCompatibilityConsumer route.
 */

#include <cmath>
#include <cstdio>
#include <limits>
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

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Bar bar(double o, double h, double l, double c, int64_t timestamp) {
    return {o, h, l, c, 1.0, timestamp};
}

class WitnessHost final : public source::PineStrategyHost {
public:
    enum class Case { ReplacementGrowth, Reentry, DeferredPercent, NoTarget };

    explicit WitnessHost(Case which) : which_(which) {
        initial_capital_ = 100000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        pyramiding_ = 10;
        margin_call_enabled_ = false;
    }

    void on_source_bar(const Bar&) override {
        switch (which_) {
            case Case::ReplacementGrowth:
                if (bar_index_ == 0) {
                    strategy_entry("E", true, 95.0, kNaN, 1.0);
                    strategy_exit("X", "E", 105.0, kNaN, kNaN, kNaN,
                                  kNaN, 100.0);
                } else if (bar_index_ == 1) {
                    // Same source id replaces pending E1 before its fill.
                    strategy_entry("E", true, 95.0, kNaN, 2.0);
                }
                break;
            case Case::Reentry:
                if (bar_index_ == 0) strategy_entry("E", true, kNaN, kNaN, 1.0);
                if (bar_index_ == 1) strategy_entry("E", true, kNaN, kNaN, 2.0);
                if (bar_index_ == 2) strategy_exit("X", "E", 105.0, kNaN,
                                                    kNaN, kNaN, kNaN, 100.0);
                break;
            case Case::DeferredPercent:
                if (bar_index_ == 0) {
                    // The exit is submitted while flat, but its named parent
                    // is a live pending entry, which is the legacy deferred
                    // bracket shape (not an unbound invalid from_entry).
                    strategy_entry("E", true, 95.0, kNaN, 4.0);
                    strategy_exit("X", "E", 105.0, kNaN, kNaN, kNaN,
                                  kNaN, 50.0);
                }
                break;
            case Case::NoTarget:
                if (bar_index_ == 0) {
                    strategy_entry("NEVER", true, 50.0, kNaN, 1.0);
                    strategy_exit("X", "NEVER", 105.0, kNaN, kNaN, kNaN,
                                  kNaN, 100.0);
                    strategy_close("NEVER");  // target==0: adapter must submit nothing.
                }
                break;
        }
    }

    int pending() const { return pending_order_count(); }

private:
    Case which_;
};

void replacement_growth() {
    WitnessHost host(WitnessHost::Case::ReplacementGrowth);
    const Bar bars[] = {
        bar(100, 100, 100, 100, 1000),
        bar(100, 100, 100, 100, 2000),
        bar(100, 100, 94, 96, 3000),
        bar(96, 106, 96, 105, 4000),
    };
    host.run(bars, 4);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const Trade& t = host.get_trade(0);
        CHECK(t.entry_id == "E" && t.exit_id == "X");
        CHECK(t.entry_price == 95.0 && t.exit_price == 105.0);
        CHECK(t.qty == 2.0);  // E1=1 replaced by E2=2; deferred X grows to E2.
    }
}

void same_id_reentry() {
    WitnessHost host(WitnessHost::Case::Reentry);
    const Bar bars[] = {
        bar(100, 100, 100, 100, 1000),
        bar(100, 100, 94, 96, 2000),
        bar(96, 106, 96, 105, 3000),
        bar(105, 105, 105, 105, 4000),
    };
    host.run(bars, 4);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    if (host.trade_count() == 2) {
        const Trade& first = host.get_trade(0);
        const Trade& second = host.get_trade(1);
        CHECK(first.entry_id == "E" && second.entry_id == "E");
        CHECK(first.qty == 1.0 && second.qty == 2.0);
        CHECK(first.exit_id == "X" && second.exit_id == "X");
        CHECK(first.exit_price == 105.0 && second.exit_price == 105.0);
    }
}

void flat_percent_resolves_at_fill() {
    WitnessHost host(WitnessHost::Case::DeferredPercent);
    const Bar bars[] = {
        bar(100, 100, 100, 100, 1000),
        bar(100, 100, 94, 96, 2000),
        bar(96, 106, 96, 105, 3000),
        bar(105, 105, 105, 105, 4000),
    };
    host.run(bars, 4);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const Trade& t = host.get_trade(0);
        CHECK(t.qty == 2.0);  // 50 percent of the eventual 4-unit E cohort.
        CHECK(t.entry_price == 95.0 && t.exit_price == 105.0);
    }
}

void never_opened_target_stays_deferred_and_close_drops() {
    WitnessHost host(WitnessHost::Case::NoTarget);
    const Bar bars[] = {
        bar(100, 100, 100, 100, 1000),
        bar(100, 100, 100, 100, 2000),
        bar(100, 100, 100, 100, 3000),
    };
    host.run(bars, 3);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 0);
    CHECK(host.pending() == 2);  // pending parent + deferred exit; close("NEVER") is dropped.
}
}  // namespace

int main() {
    replacement_growth();
    same_id_reentry();
    flat_percent_resolves_at_fill();
    never_opened_target_stays_deferred_and_close_drops();
    std::printf("R4-D deferred-ANY oracle: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
