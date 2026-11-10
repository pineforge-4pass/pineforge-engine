// Exits placed against held lots end when an entry reverses those lots.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

namespace {
using namespace pineforge;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kPrice = 100.0;
constexpr double kOldQty = 3.0;
constexpr double kNewQty = 2.0;
constexpr double kPercent = 50.0;
constexpr double kLimitDistance = 1.0;
constexpr double kStopDistance = 2.0;
constexpr int kReverseBar = 2;
constexpr int kTouchBar = kReverseBar + 1;
constexpr int kLastBar = kTouchBar + 1;
constexpr std::int64_t kStep = 60000;

enum class Placement { WithReversal, AfterClose, OtherEntry, BeforeReversal };

struct Scenario {
    const char* name;
    bool new_long;
    Placement placement;

    double direction() const { return new_long ? 1.0 : -1.0; }
    std::string new_id() const {
        return placement == Placement::OtherEntry ? "Y" : "X";
    }
    bool exits_live() const {
        return placement == Placement::AfterClose || placement == Placement::OtherEntry;
    }
};

const char* scenario = "";
int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                        \
    ++checks;                                                                  \
    if (!(expr)) {                                                             \
        ++failures;                                                            \
        std::fprintf(stderr, "%s:%d [%s] %s\n", __FILE__, __LINE__, scenario,    \
                     #expr);                                                   \
    }                                                                          \
} while (false)

std::vector<Bar> feed(const Scenario& spec) {
    // Bars 0..2 are flat at P. Bar 3 opens at P and moves continuously past
    // the new side's stop P - direction * 2, to P - direction * (2 + 1).
    // Its other extreme is P, so it never touches P + direction * 1.
    // Bar 4 is flat at that final price. All prices are on the unit grid.
    const double final_price = kPrice - spec.direction() * (kStopDistance + 1.0);
    std::vector<Bar> bars;
    for (int i = 0; i <= kLastBar; ++i) {
        const double open = i <= kTouchBar ? kPrice : final_price;
        const double close = i < kTouchBar ? kPrice : final_price;
        bars.push_back({open, std::max(open, close), std::min(open, close), close,
                        1.0, (static_cast<std::int64_t>(i) + 1) * kStep});
    }
    return bars;
}

class ReversalProbe final : public source::PineStrategyHost {
public:
    explicit ReversalProbe(const Scenario& spec) : spec_(spec) {
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = true;
        config.calc_on_order_fills = false;
        config.close_entries_rule_any = false;
        config.pyramiding = 0;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(1.0);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", 1.0);
        margin_call_enabled_ = false;
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i == 0)
            strategy_entry("X", !spec_.new_long, kNaN, kNaN, kOldQty);
        if (i == 1 && spec_.placement == Placement::AfterClose)
            strategy_close("X");
        if (i == 1 && spec_.placement == Placement::BeforeReversal)
            place_exits(bar.close, -spec_.direction());
        if (i == kReverseBar) {
            before_entry = signed_position_size();
            strategy_entry(spec_.new_id(), spec_.new_long, kNaN, kNaN, kNewQty);
            if (spec_.placement != Placement::BeforeReversal)
                place_exits(bar.close, spec_.direction());
        }
    }

    double before_entry = kNaN;
    double final_units() const { return signed_position_size(); }

private:
    void place_exits(double close, double direction) {
        const double limit = close + direction * kLimitDistance;
        const double stop = close - direction * kStopDistance;
        strategy_exit("TP", spec_.new_id(), limit, stop,
                      kNaN, kNaN, kNaN, kPercent);
        strategy_exit("SL", spec_.new_id(), kNaN, stop,
                      kNaN, kNaN, kNaN, 100.0);
    }

    const Scenario& spec_;
};

void check_trade(const Trade& got, const std::vector<Bar>& bars,
                 int entry_bar, int exit_bar, double exit_price, double qty,
                 bool is_long, const std::string& entry_id,
                 const std::string& exit_id, bool open_at_end) {
    // With no fees or slippage, entry = the signal bar's close and
    // PnL = (long ? 1 : -1) * (exit - entry) * quantity * pointvalue(1).
    const double entry_price = bars[entry_bar].close;
    const double pnl = (is_long ? 1.0 : -1.0) * (exit_price - entry_price) * qty;
    CHECK(got.entry_id == entry_id);
    CHECK(got.exit_id == exit_id);
    CHECK(got.entry_bar_index == entry_bar);
    CHECK(got.exit_bar_index == exit_bar);
    CHECK(got.entry_time == bars[entry_bar].timestamp);
    CHECK(got.exit_time == bars[exit_bar].timestamp);
    CHECK(got.entry_price == entry_price);
    CHECK(got.exit_price == exit_price);
    CHECK(got.qty == qty);
    CHECK(got.pnl == pnl);
    CHECK(got.is_long == is_long);
    CHECK(got.open_at_end == open_at_end);
    CHECK(got.exit_from_bracket == (exit_id == "TP" || exit_id == "SL"));
}

void run_case(const Scenario& spec) {
    scenario = spec.name;
    const auto bars = feed(spec);
    ReversalProbe probe(spec);
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    if (!probe.last_error().empty()) return;

    // The old 3 units close at P, either on bar 1's explicit close or on
    // bar 2's reversal: (P - P) * 3 = 0 in either direction.
    const bool closed_first = spec.placement == Placement::AfterClose;
    const int old_exit_bar = closed_first ? 1 : kReverseBar;
    const std::string old_exit_id = closed_first ? "__close__X" : spec.new_id();
    CHECK(probe.before_entry == (closed_first ? 0.0 : -spec.direction() * kOldQty));
    CHECK(probe.closed_trade_count() == (spec.exits_live() ? 3U : 1U));
    CHECK(probe.report_trade_count() == (spec.exits_live() ? 3 : 2));
    if (probe.closed_trade_count() < 1) return;
    check_trade(probe.get_trade(0), bars, 0, old_exit_bar,
                bars[old_exit_bar].close, kOldQty, !spec.new_long,
                "X", old_exit_id, false);

    if (spec.exits_live()) {
        // The new 2 units reserve 2 * 50 / 100 = 1 for TP, leaving
        // 2 - 1 = 1 for SL. Both stop at P - direction * 2 on bar 3:
        // direction * (-direction * 2) * 1 = -2 per closed row.
        // The old lot and these two fragments give 1 + 2 = 3 closed rows.
        const double tp_qty = kNewQty * kPercent / 100.0;
        const double sl_qty = kNewQty - tp_qty;
        const double stop = bars[kReverseBar].close - spec.direction() * kStopDistance;
        if (probe.closed_trade_count() >= 3) {
            check_trade(probe.get_trade(1), bars, kReverseBar, kTouchBar, stop,
                        tp_qty, spec.new_long, spec.new_id(), "TP", false);
            check_trade(probe.get_trade(2), bars, kReverseBar, kTouchBar, stop,
                        sl_qty, spec.new_long, spec.new_id(), "SL", false);
        }
        CHECK(probe.final_units() == spec.direction() * (kNewQty - tp_qty - sl_qty));
    } else {
        // One closed old lot plus one open new lot gives 1 + 1 = 2 report
        // rows. The new lot stays at direction * 2 units, marked at bar 4:
        // direction * (-direction * (2 + 1)) * 2 = -6.
        CHECK(probe.final_units() == spec.direction() * kNewQty);
        if (probe.report_trade_count() >= 2)
            check_trade(probe.get_report_trade(1), bars, kReverseBar, kLastBar,
                        bars.back().close, kNewQty, spec.new_long,
                        spec.new_id(), "", true);
        for (int i = 0; i < probe.report_trade_count(); ++i) {
            const auto row = probe.get_report_trade(i);
            CHECK(row.exit_id != "TP");
            CHECK(row.exit_id != "SL");
        }
    }
}
} // namespace

int main() {
    // TradingView tape sha256:638438371ac6e073b61b689d01af352db6e602230f217c75336492de4e26033e
    run_case({"E1", false, Placement::WithReversal});
    // TradingView tape sha256:638438371ac6e073b61b689d01af352db6e602230f217c75336492de4e26033e
    run_case({"E1m", true, Placement::WithReversal});
    // TradingView tape sha256:638438371ac6e073b61b689d01af352db6e602230f217c75336492de4e26033e
    run_case({"E2", false, Placement::AfterClose});
    // TradingView tape sha256:638438371ac6e073b61b689d01af352db6e602230f217c75336492de4e26033e
    run_case({"E3", false, Placement::OtherEntry});
    // The earlier exits protect the old long at limit P + 1 and stop P - 2;
    // flat bars 1 and 2 touch neither. They end with that long's reversal.
    // TradingView tape sha256:638438371ac6e073b61b689d01af352db6e602230f217c75336492de4e26033e
    run_case({"E4", false, Placement::BeforeReversal});
    return failures == 0 ? 0 : 1;
}
