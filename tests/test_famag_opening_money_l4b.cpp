/*
 * Round 12 AG-C2: rounded required money at an existing 1x opening check.
 *
 * TradingView pins: r12-c2-may28, r12-c2-jun09 and the repaired June 9
 * near1/near2/headroom-seedearly sensors. See campaign notes
 * log-20260906t030608z-5e7dd07e and log-20260906t030919z-9a77d540.
 * Small synthetic fixtures retain the pins' signal/fill prices and
 * capital constants, without loading a corpus, feed, or verifier.
 *
 * May 28: TV trims 62.32 at the long fill, not the exact-cost 62.28.
 * June 9: rounded required money exceeds exact equity by 0.000148, so TV
 * trims one contract at the short fill, then 1912.92 at the adverse high.
 * Moving the old quantity to newQ-0.01/-0.02 keeps both events. Adding
 * 0.0006 equity removes only the opening call, giving 1916.92 at the high.
 * r12-c2-may28-pricedexit independently pins the same 62.32 opening trim
 * before a pending stop at 1.13430, which closes 885063.82 on that bar
 * (TV CSV a1a4872e7f5ff6bbebf538f1186bf8d4902ef3566e1f0d6ee195982dc510a496).
 */
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "oracle_fixture_config_shim.hpp"

using namespace pineforge;

static int passed = 0;
static int failed = 0;
#define CHECK(expr) do { \
    if (expr) { ++passed; } else { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failed; \
    } \
} while (0)

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool near(double a, double b, double tolerance = 1e-6) {
    return std::abs(a - b) < tolerance;
}

class OpeningMoneyProbe : public pineforge::source::PineStrategyHost {
public:
    OpeningMoneyProbe(double capital, bool seed_long, double seed_qty,
                      int seed_bar, int reverse_bar, int flatten_bar,
                      bool same_bar_stop = false)
        : seed_long_(seed_long), seed_qty_(seed_qty), seed_bar_(seed_bar),
          reverse_bar_(reverse_bar), flatten_bar_(flatten_bar),
          same_bar_stop_(same_bar_stop) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = margin_short_ = 100.0;
        pyramiding_ = 0;
        slippage_ = 0;
        syminfo_.pointvalue = 1.0;
        set_syminfo_mintick(0.00001);
        qty_step_ = 0.01;
        process_orders_on_close_ = false;
        set_margin_call_enabled(true);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == seed_bar_)
            strategy_entry("Seed", seed_long_, kNaN, kNaN, seed_qty_, "SEED");
        if (bar_index_ == reverse_bar_) {
            strategy_close("Seed", "CLOSE");
            strategy_entry("Next", !seed_long_, kNaN, kNaN, kNaN, "NEXT");
            if (same_bar_stop_)
                strategy_exit("Stop", "Next", kNaN, 1.13430,
                              kNaN, kNaN, kNaN, 100.0, "STOP");
        }
        if (bar_index_ == flatten_bar_) strategy_close("Next", "END");
    }

    std::vector<Trade> rows() const { return trades_; }
    double remaining_position() const { return signed_position_size(); }

private:
    bool seed_long_;
    double seed_qty_;
    int seed_bar_;
    int reverse_bar_;
    int flatten_bar_;
    bool same_bar_stop_;
};

std::vector<Bar> may_bars() {
    return {
        {1.13398, 1.13398, 1.13398, 1.13398, 1, 1000},
        {1.13398, 1.13450, 1.13390, 1.13449, 1, 2000},
        {1.13450, 1.13452, 1.13424, 1.13450, 1, 3000},
        {1.13450, 1.13450, 1.13450, 1.13450, 1, 4000},
        {1.13450, 1.13450, 1.13450, 1.13450, 1, 5000},
    };
}

std::vector<Bar> june_bars() {
    return {
        {1.14106, 1.14106, 1.14106, 1.14106, 1, 1000},
        {1.14108, 1.14186, 1.14100, 1.14176, 1, 2000},
        {1.14175, 1.14182, 1.14070, 1.14085, 1, 3000},
        {1.14086, 1.14117, 1.14054, 1.14090, 1, 4000},
        {1.14088, 1.14116, 1.14056, 1.14065, 1, 5000},
        {1.14064, 1.14064, 1.14064, 1.14064, 1, 6000},
    };
}

void run(OpeningMoneyProbe& engine, const std::vector<Bar>& bars) {
    engine.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(engine.last_error().empty());
    CHECK(near(engine.remaining_position(), 0.0));
}

void may28_restore_amount(bool converted, bool same_bar_stop = false) {
    OpeningMoneyProbe engine(1004616.8129284, false, 882466.37, 0, 1, 3,
                            same_bar_stop);
    if (converted) {
        // A provider series retains the existing converted-ledger arithmetic,
        // even when its numeric rate is 1. No ten-digit rule was pinned there.
        const int64_t timestamps[] = {1000};
        const double rates[] = {1.0};
        CHECK(engine.set_account_currency_fx_series(timestamps, rates, 1));
    }
    run(engine, may_bars());
    const auto rows = engine.rows();
    CHECK(rows.size() == 3);
    if (rows.size() != 3) return;
    CHECK(rows[0].exit_comment == "CLOSE");
    CHECK(near(rows[0].qty, 882466.37));
    CHECK(rows[0].exit_time == 3000);
    CHECK(rows[1].exit_comment == "Margin call");
    CHECK(near(rows[1].entry_price, 1.13450));
    CHECK(near(rows[1].exit_price, 1.13450));
    CHECK(rows[1].entry_time == 3000 && rows[1].exit_time == 3000);
    CHECK(near(rows[1].qty, converted ? 62.28 : 62.32));
    // Composition with finding-325: an armed same-bar stop must consume the
    // same rounded remainder. This is the existing entry checkpoint moved
    // before a priced exit, not a new cursor or scheduling rule.
    CHECK(rows[2].exit_comment == (same_bar_stop ? "STOP" : "END"));
    if (same_bar_stop) {
        CHECK(rows[2].exit_time == 3000);
        CHECK(near(rows[2].exit_price, 1.13430));
    }
    CHECK(near(rows[2].qty, converted ? 885063.86 : 885063.82));
    CHECK(near(rows[1].qty + rows[2].qty, 885126.14));
}

void june09_opening_and_adverse(double capital, double seed_qty,
                               int seed_bar, bool headroom) {
    OpeningMoneyProbe engine(capital, true, seed_qty, seed_bar, 2, 4);
    run(engine, june_bars());
    const auto rows = engine.rows();
    CHECK(rows.size() == (headroom ? 3u : 4u));
    if (rows.size() != (headroom ? 3u : 4u)) return;
    CHECK(rows[0].exit_comment == "CLOSE");
    CHECK(near(rows[0].qty, seed_qty)); // proves the seed was actually admitted
    CHECK(rows[0].exit_time == 4000);
    if (!headroom) {
        CHECK(rows[1].exit_comment == "Margin call");
        CHECK(near(rows[1].qty, 1.0));
        CHECK(near(rows[1].entry_price, 1.14086));
        CHECK(near(rows[1].exit_price, 1.14086));
        CHECK(rows[1].entry_time == 4000 && rows[1].exit_time == 4000);
    }
    const auto& adverse = rows[headroom ? 1 : 2];
    CHECK(adverse.exit_comment == "Margin call");
    CHECK(near(adverse.qty, headroom ? 1916.92 : 1912.92));
    CHECK(near(adverse.entry_price, 1.14086));
    CHECK(near(adverse.exit_price, 1.14117));
    CHECK(adverse.entry_time == 4000 && adverse.exit_time == 4000);
    CHECK(rows.back().exit_comment == "END");
    CHECK(near(rows.back().qty, headroom ? 880167.48 : 880170.48));
    double next_total = 0.0;
    for (size_t i = 1; i < rows.size(); ++i) next_total += rows[i].qty;
    CHECK(near(next_total, 882084.40));
}
} // namespace

int main() {
    may28_restore_amount(false);
    may28_restore_amount(true);
    may28_restore_amount(false, true);
    may28_restore_amount(true, true);
    june09_opening_and_adverse(1007119.8502264001, 882068.96, 1, false);
    june09_opening_and_adverse(1006528.8674178, 882084.39, 0, false);
    june09_opening_and_adverse(1006528.8674156, 882084.38, 0, false);
    june09_opening_and_adverse(1006528.8680178, 882084.39, 0, true);
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
