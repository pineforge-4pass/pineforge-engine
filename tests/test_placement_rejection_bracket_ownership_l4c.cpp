#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"
// A placement-level whole-order rejection never acquires the old position's
// priced exits. An admitted reversal declined at the opening gap still does.
// Covered TV controls r31-r5-stop-{z-tie,g-tie,g-gap,g-none} (2026-09-07):
// short 870000 @ 1.13523; standing stop 1.13530. The two rule-5 ties keep
// that stop live, while capital +0.0005 at the adverse-gap signal kills it.
// r31-r5-limit-{g-tie,g-gap,g-none} pins the same distinction for a standing
// profit limit at 1.13165. Neither control family has any margin-call slice.
// These compact synthetic schedules preserve the pinned account budgets;
// they contain no indicator, symbol, date, or strategy-specific dispatch.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;
namespace {
int passed = 0;
int failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (false)
const double kNa = std::numeric_limits<double>::quiet_NaN();
enum class Mode { ZeroGapPlacement, AdverseGapPlacement, FillGap, NoReversal };
enum class Leg { Stop, Limit };
Bar bar(int i, double o, double h, double l, double c) {
    Bar b;
    b.timestamp = 1000000LL + i * 900000LL;
    b.open = o; b.high = h; b.low = l; b.close = c; b.volume = 1.0;
    return b;
}
std::vector<Bar> schedule(Mode mode, Leg leg) {
    const bool zero = mode == Mode::ZeroGapPlacement;
    return {
        bar(0, 1.13596, 1.13622, 1.13513, 1.13524),
        bar(1, 1.13523, 1.13523, 1.13378, 1.13388),
        zero ? bar(2, 1.13375, 1.13448, 1.13345, 1.13384)
             : bar(2, 1.13232, 1.13263, 1.13174, 1.13207),
        zero ? bar(3, 1.13384, 1.13391, 1.13273, 1.13276)
             : bar(3, 1.13209, 1.13350, 1.13196, 1.13349),
        leg == Leg::Stop ? bar(4, 1.13368, 1.13546, 1.13332, 1.13476)
                         : bar(4, 1.13189, 1.13282, 1.13156, 1.13231),
    };
}
class Probe : public pineforge::source::PineStrategyHost {
    Mode mode_;
    Leg leg_;
public:
    double signal_equity = kNa;
    Probe(Mode mode, Leg leg) : mode_(mode), leg_(leg) {
        initial_capital_ = mode == Mode::ZeroGapPlacement ? 998790.695916
            : mode == Mode::AdverseGapPlacement ? 997250.797032 : 997250.797532;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        margin_long_ = margin_short_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
        qty_step_ = 0.01;
        syminfo_.pointvalue = 1.0;
        set_syminfo_mintick(0.00001);
        set_margin_call_enabled(true);
    }
    void on_source_bar(const Bar& b) override {
        if (bar_index_ == 0) {
            strategy_entry("Owned", false, kNa, kNa, 870000.0);
            strategy_exit("Standing", "Owned",
                          leg_ == Leg::Limit ? 1.13165 : kNa,
                          leg_ == Leg::Stop ? 1.13530 : kNa);
        }
        if (bar_index_ == 2) {
            signal_equity = current_equity() + open_profit(b.close);
            if (mode_ != Mode::NoReversal) strategy_entry("Attempt", true);
        }
    }
    const std::vector<Trade>& closed() const { return trades_; }
    double position() const { return signed_position_size(); }
};
bool near(double a, double b, double eps = 1e-8) {
    return std::abs(a - b) < eps;
}
void check(Mode mode, double equity, bool exit_lives, Leg leg = Leg::Stop) {
    const auto input = schedule(mode, leg);
    Probe p(mode, leg);
    p.run(input.data(), static_cast<int>(input.size()));
    CHECK(near(p.signal_equity, equity));
    if (!exit_lives) {
        CHECK(p.closed().empty());
        CHECK(near(p.position(), -870000.0));
        return;
    }
    CHECK(p.closed().size() == 1);
    CHECK(near(p.position(), 0.0));
    if (p.closed().size() != 1) return;
    const auto& t = p.closed()[0];
    CHECK(!t.is_long);
    CHECK(t.entry_id == "Owned");
    CHECK(t.exit_id == "Standing");
    CHECK(near(t.qty, 870000.0));
    CHECK(near(t.entry_price, 1.13523));
    CHECK(near(t.exit_price, leg == Leg::Stop ? 1.13530 : 1.13165));
    CHECK(t.exit_time == input[4].timestamp);
    CHECK(t.exit_comment != "Margin call");
}
}
int main() {
    check(Mode::ZeroGapPlacement, 999999.995916, true);
    check(Mode::AdverseGapPlacement, 999999.997032, true);
    check(Mode::FillGap, 999999.997532, false);
    check(Mode::NoReversal, 999999.997532, true);
    check(Mode::AdverseGapPlacement, 999999.997032, true, Leg::Limit);
    check(Mode::FillGap, 999999.997532, false, Leg::Limit);
    check(Mode::NoReversal, 999999.997532, true, Leg::Limit);
    std::printf("placement rejection bracket ownership: %d passed / %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
