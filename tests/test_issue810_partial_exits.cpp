/*
 * Issue 810: partial strategy.exit reservations must survive the parent fill,
 * later re-issues, and same-id pyramiding.
 *
 * The quantities and control substitutions come from the reporter's public
 * issue. The bars below isolate the reservation lifecycles with real source
 * commands and fills; they are not a replacement TradingView tape.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kEntryQty = 0.02;
constexpr double kTolerance = 1e-12;

int tests_passed = 0;
int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

bool near(double left, double right) {
    return std::fabs(left - right) <= kTolerance;
}

Bar bar(int index, double open, double high, double low, double close) {
    Bar value;
    value.open = open;
    value.high = high;
    value.low = low;
    value.close = close;
    value.volume = 1000.0;
    value.timestamp = 1'000'000 + static_cast<std::int64_t>(index) * 60'000;
    return value;
}

enum class Case {
    ARelative,
    APriceControl,
    BExplicitQty,
    BPercentControl,
    CPercent,
    CFullControl,
};

bool is_a(Case value) {
    return value == Case::ARelative || value == Case::APriceControl;
}

bool is_b(Case value) {
    return value == Case::BExplicitQty || value == Case::BPercentControl;
}

bool is_c(Case value) {
    return value == Case::CPercent || value == Case::CFullControl;
}

class Issue810Probe final : public source::PineStrategyHost {
public:
    struct Opening {
        std::uint64_t origin;
        std::string id;
        int bar;
        double units;
    };

    explicit Issue810Probe(Case test_case) : test_case_(test_case) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.initial_capital = 10'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = kEntryQty;
        config.pyramiding = is_c(test_case) ? 2 : 1;
        config.process_orders_on_close = is_a(test_case);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_mintick(0.01);
        set_syminfo_metadata("qty_step", 0.00001);
    }

    double signed_position() const {
        return signed_position_size();
    }

    std::vector<Opening> openings;
    std::vector<double> pending_tp_quantities;
    bool pending_observation_ok = true;

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext& context) override {
        if (std::abs(event.opened_units) > 0.0) {
            openings.push_back({
                event.handle().incarnation,
                event.request().label,
                static_cast<int>(context.coordinate.interval_index),
                std::abs(event.opened_units),
            });
        }
    }

    void on_source_bar(const Bar&) override {
        if (is_a(test_case_)) {
            if (bar_index_ != 0) return;
            strategy_entry("Long", true, kNaN, kNaN, kEntryQty);
            if (test_case_ == Case::ARelative) {
                // Issue A: reporter's 50% tick exit is placed before the
                // same-bar entry fills.
                strategy_exit("Half", "Long", kNaN, kNaN, kNaN, kNaN, kNaN,
                              50.0, "", kNaN, "", 50'000.0, kNaN);
            } else {
                // One-change control for A: the same target is absolute.
                strategy_exit("Half", "Long", 44415.71, kNaN, kNaN, kNaN, kNaN,
                              50.0);
            }
            strategy_exit("Rest", "Long", kNaN, 42915.71, kNaN, kNaN, kNaN,
                          100.0);
            return;
        }

        if (is_b(test_case_)) {
            if (bar_index_ == 0) {
                strategy_entry("Long", true, kNaN, kNaN, kEntryQty);
                return;
            }
            if (signed_position() <= 0.0) return;
            if (test_case_ == Case::BExplicitQty) {
                // Issue B: the explicit 0.01 partial is reissued on every
                // live bar, before the full sibling.
                strategy_exit("Half", "Long", 101.0, kNaN, kNaN, kNaN, kNaN,
                              100.0, "", 0.01);
            } else {
                // One-change control for B: the same partial is percentage
                // sized instead of explicitly sized.
                strategy_exit("Half", "Long", 101.0, kNaN, kNaN, kNaN, kNaN,
                              50.0);
            }
            strategy_exit("Rest", "Long", 102.0, kNaN, kNaN, kNaN, kNaN,
                          100.0);
            return;
        }

        if (bar_index_ == 0 || bar_index_ == 2)
            strategy_entry("Long", true, kNaN, kNaN, kEntryQty);

        if (test_case_ == Case::CPercent && bar_index_ == 4) {
            for (int index = 0; index < pending_order_count(); ++index) {
                pf_pending_order_v1_t row{};
                if (observe_pending_copy_v1(index, &row) != 0) {
                    pending_observation_ok = false;
                } else if (std::string(row.id) == "TP") {
                    pending_tp_quantities.push_back(row.qty);
                }
            }
            std::sort(pending_tp_quantities.begin(), pending_tp_quantities.end());
        }

        // Issue C reissues the one named exit as the second same-ID origin is
        // added. The control omits qty_percent and is therefore full-sized.
        strategy_exit("TP", "Long", 101.0, kNaN, kNaN, kNaN, kNaN,
                      test_case_ == Case::CPercent ? 50.0 : 100.0);
    }

private:
    Case test_case_;
};

std::vector<Bar> bars_for(Case test_case) {
    if (is_a(test_case)) {
        return {
            bar(0, 43915.71, 43915.71, 43915.71, 43915.71),
            bar(1, 43915.71, 44415.71, 43800.0, 44400.0),
            bar(2, 44400.0, 44410.0, 42915.71, 43000.0),
            bar(3, 43000.0, 43010.0, 42950.0, 43000.0),
        };
    }
    if (is_b(test_case)) {
        return {
            bar(0, 100.0, 100.0, 100.0, 100.0),
            bar(1, 100.0, 100.5, 99.0, 100.0),
            bar(2, 100.0, 100.5, 99.0, 100.0),
            bar(3, 100.0, 100.5, 99.0, 100.0),
            bar(4, 100.0, 101.0, 99.0, 100.0),
            bar(5, 100.0, 102.0, 99.0, 100.0),
            bar(6, 100.0, 101.0, 99.0, 100.0),
        };
    }
    return {
        bar(0, 100.0, 100.0, 100.0, 100.0),
        bar(1, 100.0, 100.0, 100.0, 100.0),
        bar(2, 100.0, 100.0, 100.0, 100.0),
        bar(3, 100.0, 100.0, 100.0, 100.0),
        bar(4, 100.0, 100.0, 100.0, 100.0),
        bar(5, 100.0, 101.0, 99.0, 100.0),
        bar(6, 100.0, 100.0, 100.0, 100.0),
    };
}

void check_issue_a(Case test_case) {
    std::printf("case A %s\n", test_case == Case::ARelative ? "reported" : "price-control");
    Issue810Probe probe(test_case);
    const auto bars = bars_for(test_case);
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(probe.openings.size() == 1);
    if (probe.openings.size() == 1) {
        CHECK(probe.openings[0].id == "Long");
        CHECK(probe.openings[0].bar == 0);
        CHECK(near(probe.openings[0].units, kEntryQty));
    }
    CHECK(probe.trade_count() == 2);
    int half_rows = 0;
    int rest_rows = 0;
    double closed = 0.0;
    for (int index = 0; index < probe.trade_count(); ++index) {
        const Trade& trade = probe.get_trade(index);
        CHECK(trade.entry_id == "Long");
        CHECK(trade.entry_bar_index == 0);
        CHECK(near(trade.qty, 0.01));
        closed += trade.qty;
        if (trade.exit_id == "Half") {
            ++half_rows;
            CHECK(trade.exit_bar_index == 1);
            CHECK(near(trade.exit_price, 44415.71));
        } else if (trade.exit_id == "Rest") {
            ++rest_rows;
            CHECK(trade.exit_bar_index == 2);
            CHECK(near(trade.exit_price, 42915.71));
        } else {
            CHECK(false);
        }
    }
    CHECK(half_rows == 1);
    CHECK(rest_rows == 1);
    CHECK(near(closed, kEntryQty));
    CHECK(near(probe.signed_position(), 0.0));
}

void check_issue_b(Case test_case) {
    std::printf("case B %s\n", test_case == Case::BExplicitQty ? "reported" : "percent-control");
    Issue810Probe probe(test_case);
    const auto bars = bars_for(test_case);
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(probe.openings.size() == 1);
    if (probe.openings.size() == 1) {
        CHECK(probe.openings[0].id == "Long");
        CHECK(probe.openings[0].bar == 1);
        CHECK(near(probe.openings[0].units, kEntryQty));
    }
    CHECK(probe.trade_count() == 2);
    int half_rows = 0;
    int rest_rows = 0;
    double closed = 0.0;
    for (int index = 0; index < probe.trade_count(); ++index) {
        const Trade& trade = probe.get_trade(index);
        CHECK(trade.entry_id == "Long");
        CHECK(trade.entry_bar_index == 1);
        CHECK(near(trade.qty, 0.01));
        closed += trade.qty;
        if (trade.exit_id == "Half") {
            ++half_rows;
            CHECK(trade.exit_bar_index == 4);
            CHECK(near(trade.exit_price, 101.0));
        } else if (trade.exit_id == "Rest") {
            ++rest_rows;
            CHECK(trade.exit_bar_index == 5);
            CHECK(near(trade.exit_price, 102.0));
        } else {
            CHECK(false);
        }
    }
    CHECK(half_rows == 1);
    CHECK(rest_rows == 1);
    CHECK(near(closed, kEntryQty));
    CHECK(near(probe.signed_position(), 0.0));
}

void check_issue_c(Case test_case) {
    std::printf("case C %s\n", test_case == Case::CPercent ? "reported" : "full-control");
    Issue810Probe probe(test_case);
    const auto bars = bars_for(test_case);
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(probe.openings.size() == 2);
    if (probe.openings.size() == 2) {
        CHECK(probe.openings[0].id == "Long");
        CHECK(probe.openings[1].id == "Long");
        CHECK(probe.openings[0].bar == 1);
        CHECK(probe.openings[1].bar == 3);
        CHECK(near(probe.openings[0].units, kEntryQty));
        CHECK(near(probe.openings[1].units, kEntryQty));
        CHECK(probe.openings[0].origin != probe.openings[1].origin);
    }

    const bool partial = test_case == Case::CPercent;
    if (partial) {
        CHECK(probe.pending_observation_ok);
        CHECK(probe.pending_tp_quantities.size() == 2);
        if (probe.pending_tp_quantities.size() == 2) {
            CHECK(near(probe.pending_tp_quantities[0], 0.01));
            CHECK(near(probe.pending_tp_quantities[1], 0.01));
        }
    }

    CHECK(probe.trade_count() > 0);
    double closed = 0.0;
    double from_first = 0.0;
    double from_second = 0.0;
    for (int index = 0; index < probe.trade_count(); ++index) {
        const Trade& trade = probe.get_trade(index);
        CHECK(trade.entry_id == "Long");
        CHECK(trade.exit_id == "TP");
        CHECK(trade.exit_bar_index == 5);
        CHECK(near(trade.exit_price, 101.0));
        if (trade.entry_bar_index == 1) {
            from_first += trade.qty;
        } else if (trade.entry_bar_index == 3) {
            from_second += trade.qty;
        } else {
            CHECK(false);
        }
        closed += trade.qty;
    }
    CHECK(near(closed, partial ? 0.02 : 0.04));
    if (partial) {
        // Under the reporter's default FIFO rule, both 0.01 reservations
        // book against the first lot, while 0.02 remains open.
        CHECK(near(from_first, 0.02));
        CHECK(near(from_second, 0.0));
        CHECK(near(probe.signed_position(), 0.02));
    } else {
        CHECK(near(from_first, 0.02));
        CHECK(near(from_second, 0.02));
        CHECK(near(probe.signed_position(), 0.0));
    }
}

void test_reporter_shapes_and_controls() {
    std::printf("test_reporter_shapes_and_controls\n");
    check_issue_a(Case::ARelative);
    check_issue_a(Case::APriceControl);
    check_issue_b(Case::BExplicitQty);
    check_issue_b(Case::BPercentControl);
    check_issue_c(Case::CPercent);
    check_issue_c(Case::CFullControl);
}

}  // namespace

int main() {
    test_reporter_shapes_and_controls();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}

