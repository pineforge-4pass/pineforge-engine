// Carried 100%-margin money rounding at the next open precedes a resting
// priced exit that is not marketable there. Covered TradingView controls:
// eur7-jake-first-fixed, funded, open-race, half, and stop. This synthetic
// order schedule has no strategy signals; the price/quantity ownership is
// the contract. An already-marketable exit retains its own opening priority.
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
constexpr double kQty = 892347.23;
const double kNa = std::numeric_limits<double>::quiet_NaN();
Bar make_bar(int i, double o, double h, double l, double c) {
    Bar out;
    out.timestamp = 1744306200000LL + i * 900000LL;
    out.open = o; out.high = h; out.low = l; out.close = c; out.volume = 1.0;
    return out;
}
std::vector<Bar> bars() {
    return {
        make_bar(0, 1.11788, 1.12110, 1.11776, 1.12064),
        make_bar(1, 1.12064, 1.12100, 1.11990, 1.12099),
        make_bar(2, 1.12103, 1.12169, 1.12012, 1.12160),
        make_bar(3, 1.12160, 1.12395, 1.12152, 1.12372),
        make_bar(4, 1.12373, 1.12418, 1.12346, 1.12362),
        make_bar(5, 1.12362, 1.12377, 1.12090, 1.12156),
        make_bar(6, 1.12154, 1.12198, 1.11798, 1.11866),
    };
}
enum class Mode { Bracket, Funded, OpenRace, NonpositiveOpen, Half, Stop, Disabled };
class Probe : public pineforge::source::PineStrategyHost {
    Mode mode_;
public:
    double script_size = kNa;
    explicit Probe(Mode mode) : mode_(mode) {
        initial_capital_ = mode == Mode::Funded ? 1000000.0001 : 1000000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100;
        pyramiding_ = 10;
        margin_long_ = margin_short_ = 100;
        commission_value_ = 0;
        slippage_ = 0;
        qty_step_ = 0.01;
        set_syminfo_mintick(0.00001);
        syminfo_.pointvalue = 1;
        set_margin_call_enabled(mode != Mode::Disabled);
    }
    void on_source_bar(const Bar& b) override {
        if (bar_index_ == 0) {
            strategy_entry("Owned", true);
            if (mode_ != Mode::OpenRace && mode_ != Mode::NonpositiveOpen
                && mode_ != Mode::Stop)
                strategy_exit("Bracket", "Owned", mode_ == Mode::Half ? 1.2 : b.close * 1.003,
                              mode_ == Mode::Half ? kNa : b.close * 0.998);
        }
        if (bar_index_ == 3) {
            if (mode_ == Mode::OpenRace) strategy_exit("AtOpen", "Owned", 1.12373, kNa);
            if (mode_ == Mode::NonpositiveOpen)
                strategy_exit("FiniteAtOpen", "Owned", -1.0, 1.11839);
            if (mode_ == Mode::Stop) strategy_exit("Stop", "Owned", kNa, 1.12365);
        }
        if (bar_index_ == 4) {
            script_size = signed_position_size();
            if (mode_ == Mode::Half) strategy_close("Owned", "half", kNa, 50.0);
        }
        if (bar_index_ == 5 && mode_ == Mode::Half) strategy_close_all();
    }
    const std::vector<Trade>& closed() const { return trades_; }
};
bool near(double a, double b, double eps = 1e-8) { return std::abs(a-b) < eps; }
void check_priced_exit(Mode mode, bool call, double price) {
    auto input = bars();
    Probe p(mode);
    p.run(input.data(), static_cast<int>(input.size()));
    const auto& out = p.closed();
    CHECK(out.size() == (call ? 2U : 1U));
    if (out.size() != (call ? 2U : 1U)) return;
    if (call) {
        CHECK(out[0].exit_comment == "Margin call");
        CHECK(out[0].qty == 1.0);
        CHECK(out[0].exit_time == input[4].timestamp);
        CHECK(near(out[0].exit_price, 1.12373));
        CHECK(near(out[0].max_runup, 0.00331));
        CHECK(near(out[0].max_drawdown, 0.00074));
    }
    CHECK(near(out.back().qty, kQty - (call ? 1.0 : 0.0)));
    CHECK(near(out.back().exit_price, price));
    CHECK(out.back().exit_time == input[4].timestamp);
}
void check_script_after_open_call() {
    const auto input = bars();
    Probe p(Mode::Half);
    p.run(input.data(), static_cast<int>(input.size()));
    CHECK(near(p.script_size, 892346.23));
    const auto& out = p.closed();
    CHECK(out.size() == 3);
    if (out.size() != 3) return;
    CHECK(out[0].exit_comment == "Margin call");
    CHECK(out[0].qty == 1);
    CHECK(out[0].exit_time == input[4].timestamp);
    CHECK(near(out[0].exit_price, 1.12373));
    CHECK(out[1].exit_comment == "half");
    CHECK(near(out[1].qty, 446173.11));
    CHECK(out[1].exit_time == input[5].timestamp);
    CHECK(near(out[2].qty, 446173.12));
    CHECK(out[2].exit_time == input[6].timestamp);
}

// The existing high-value fractional class excludes priced-origin lots.
// A bar can cross the one-account-unit lot-value boundary; evaluating only O
// must not change which existing class the actual chart bar belongs to.
class BoundaryProbe : public pineforge::source::PineStrategyHost {
public:
    static constexpr double qty = 100001.1;
    static constexpr double entry_price = 9.9999;
    BoundaryProbe() {
        initial_capital_ = qty * entry_price + 0.00001;
        qty_step_ = 0.1;
        set_syminfo_mintick(0.00001);
        syminfo_.pointvalue = 1.0;
        margin_long_ = margin_short_ = 100.0;
        commission_value_ = 0.0;
        pyramiding_ = 0;
    }
    void on_source_bar(const Bar& current) override {
        if (bar_index_ != 0) return;
        // A priced lot born at the prior close has no earlier path to mark.
        position_side_ = PositionSide::LONG;
        position_qty_ = qty;
        position_entry_price_ = entry_price;
        position_entry_time_ = current.timestamp;
        position_entry_count_ = 1;
        position_open_bar_ = 0;
        PyramidEntry entry{};
        entry.price = entry_price; entry.qty = qty;
        entry.time = current.timestamp; entry.entry_id = "Boundary";
        entry.entry_bar_index = 0; entry.entry_path_position = 3.0;
        entry.entry_commission_account = 0.0;
        pyramid_entries_.push_back(entry);
        strategy_exit("Resting", "Boundary", 11.0, 9.0);
    }
    const std::vector<Trade>& closed() const { return trades_; }
    double position() const { return signed_position_size(); }
};
void check_original_money_scope_is_preserved() {
    const std::vector<Bar> input = {
        make_bar(0, 9.9999, 9.9999, 9.9999, 9.9999),
        make_bar(1, 9.99995, 10.0002, 9.9998, 10.0001),
    };
    BoundaryProbe p;
    p.run(input.data(), static_cast<int>(input.size()));
    CHECK(p.closed().empty());
    CHECK(near(p.position(), BoundaryProbe::qty));
}
}
int main() {
    check_priced_exit(Mode::Bracket, true, 1.12401);
    // The funded control has a rounding deficit at the final CLOSE, after
    // the take-profit has already filled. Do not pre-process that future point.
    check_priced_exit(Mode::Funded, false, 1.12401);
    check_priced_exit(Mode::OpenRace, false, 1.12373);
    // A finite nonpositive limit is still marketable at this positive open;
    // a valid stop sibling must not hide its established opening priority.
    check_priced_exit(Mode::NonpositiveOpen, false, 1.12373);
    check_priced_exit(Mode::Stop, true, 1.12365);
    check_priced_exit(Mode::Disabled, false, 1.12401);
    check_script_after_open_call();
    check_original_money_scope_is_preserved();
    std::printf("open money before priced exit: %d passed / %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
