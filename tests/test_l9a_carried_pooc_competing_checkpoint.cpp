#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

#define pending_orders_ source_pending_view()

// Fable delta-2 P0-A witness (A42 corrected): a carried POOC short with a
// competing pending entry-like order takes no open/path margin slice on that
// bar; the close checkpoint runs after the bar's market fills
// (ab9714be pine_fills.cpp:1172-1230, :2462-2523; pine_scheduler.cpp:260-278).
// Every literal below is the ab9714be output of the same probe
// (EV tasks/r4-d/fable-delta3-probes/mc, probe_base).
// A carried short's completed adverse-path liquidation is visible to the
// process_orders_on_close script, before a close or reversal sizes its order.
// Compact command fixtures use synthetic timestamps, with quantities/prices
// independently pinned by the R26 bare, reversal, half, funded and trail TV
// controls. The original historical probe remains unchanged.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include <initializer_list>

using namespace pineforge;
namespace {
constexpr double qnan = std::numeric_limits<double>::quiet_NaN();
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }

enum class Action { HOLD, REVERSE, HALF };
class CarriedShort : public pineforge::source::PineStrategyHost {
public:
    Action action;
    bool trail = false;
    bool parked_entry = false;
    double first_view = qnan, second_view = qnan, final_view = qnan;
    std::size_t second_closed = 0;
    explicit CarriedShort(Action value, double capital = 1392521.546177, double price_scale = 1.0)
        : action(value) {
        initial_capital_ = capital * price_scale;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        qty_step_ = 0.00001;
        syminfo_mintick_ = 0.01 * price_scale;
        syminfo_.pointvalue = 1.0;
        margin_long_ = margin_short_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
        process_orders_on_close_ = true;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("S", false, qnan, qnan, 12.60172);
        if (bar_index_ == 1) first_view = signed_position_size();
        if (bar_index_ == 2) {
            second_view = signed_position_size();
            second_closed = trades_.size();
            if (action == Action::REVERSE) strategy_entry("L", true, qnan, qnan, 2.0);
            if (action == Action::HALF) strategy_close("S", "half", qnan, 50.0);
        }
        if (trail) strategy_exit("Trail", "S", qnan, qnan, 1000.0, 1000.0);
        if (parked_entry && bar_index_ == 0) strategy_entry("Parked", true, 1.0, qnan, 0.001);
        if (bar_index_ == 3) { final_view = signed_position_size(); strategy_close_all(); }
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

const std::vector<Bar> bars = {
    {110727.28, 110920.00, 110502.44, 110502.45, 1, 1000},
    {110502.44, 110675.31, 110500.00, 110675.30, 1, 2000},
    {110675.31, 111326.20, 110666.66, 110981.97, 1, 3000},
    {110981.98, 111168.00, 110818.18, 110820.93, 1, 4000},
};

} // namespace

struct Variant : CarriedShort {
    int parked_bar; bool cancel_at2;
    Variant(int pb, bool c2) : CarriedShort(Action::HOLD), parked_bar(pb), cancel_at2(c2) {}
    void on_source_bar(const Bar&) override {
        if (bar_index_ == parked_bar) strategy_entry("Parked", true, 1.0, qnan, 0.001);
        if (cancel_at2 && bar_index_ == 2) strategy_cancel("Parked");
        if (bar_index_ == 0) strategy_entry("S", false, qnan, qnan, 12.60172);
        if (bar_index_ == 1) first_view = signed_position_size();
        if (bar_index_ == 2) { second_view = signed_position_size(); second_closed = trades_.size(); }
        if (bar_index_ == 3) { final_view = signed_position_size(); strategy_close_all(); }
    }
};

struct RowLiteral { const char* exit_id; long long exit_time; double qty; double price; };

static void expect(const char* tag, CarriedShort& e, double first, double second, double final_view,
                   std::initializer_list<RowLiteral> rows) {
    std::printf("%s\n", tag);
    CHECK(near(e.first_view, first));
    CHECK(near(e.second_view, second));
    CHECK(near(e.final_view, final_view));
    CHECK(e.rows().size() == rows.size());
    std::size_t i = 0;
    for (const auto& r : rows) {
        if (i >= e.rows().size()) break;
        const auto& t = e.rows()[i++];
        CHECK(t.exit_id == r.exit_id);
        CHECK(static_cast<long long>(t.exit_time) == r.exit_time);
        CHECK(near(t.qty, r.qty));
        CHECK(std::abs(t.exit_price - r.price) < 1e-2);
    }
}

int main() {
    { CarriedShort e(Action::REVERSE); e.run(bars.data(), static_cast<int>(bars.size())); expect("reverse, no parked", e, -12.44432, -12.33168, 2.00000, {{"__margin_call__", 2000, 0.15740, 110675.31}, {"__margin_call__", 3000, 0.11264, 111326.20}, {"L", 3000, 12.33168, 110981.97}, {"__close__", 4000, 2.00000, 110820.93}}); }
    { CarriedShort e(Action::REVERSE); e.parked_entry = true; e.run(bars.data(), static_cast<int>(bars.size())); expect("reverse, parked@0 (fixture)", e, -12.60172, -12.44432, 2.00000, {{"__margin_call__", 2000, 0.15740, 110675.31}, {"L", 3000, 12.44432, 110981.97}, {"__close__", 4000, 2.00000, 110820.93}}); }
    { Variant e(0, false); e.run(bars.data(), static_cast<int>(bars.size())); expect("hold, parked@0", e, -12.60172, -12.44432, -12.33168, {{"__margin_call__", 2000, 0.15740, 110675.31}, {"__margin_call__", 3000, 0.11264, 111326.20}, {"__close__", 4000, 12.33168, 110820.93}}); }
    { Variant e(1, false); e.run(bars.data(), static_cast<int>(bars.size())); expect("hold, parked@1 (after bar1 slice)", e, -12.44432, -12.44432, -12.33168, {{"__margin_call__", 2000, 0.15740, 110675.31}, {"__margin_call__", 3000, 0.11264, 111326.20}, {"__close__", 4000, 12.33168, 110820.93}}); }
    { Variant e(0, true); e.run(bars.data(), static_cast<int>(bars.size())); expect("hold, parked@0, cancel@2", e, -12.60172, -12.44432, -12.33168, {{"__margin_call__", 2000, 0.15740, 110675.31}, {"__margin_call__", 3000, 0.11264, 111326.20}, {"__close__", 4000, 12.33168, 110820.93}}); }
    { Variant e(2, false); e.run(bars.data(), static_cast<int>(bars.size())); expect("hold, parked@2", e, -12.44432, -12.33168, -12.33168, {{"__margin_call__", 2000, 0.15740, 110675.31}, {"__margin_call__", 3000, 0.11264, 111326.20}, {"__close__", 4000, 12.33168, 110820.93}}); }
    { Variant e(9, false); e.run(bars.data(), static_cast<int>(bars.size())); expect("hold, never parked", e, -12.44432, -12.33168, -12.33168, {{"__margin_call__", 2000, 0.15740, 110675.31}, {"__margin_call__", 3000, 0.11264, 111326.20}, {"__close__", 4000, 12.33168, 110820.93}}); }
    std::printf("test_l9a_carried_pooc_competing_checkpoint: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
