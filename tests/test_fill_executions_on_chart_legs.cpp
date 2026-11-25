// Count script bodies independently of broker instructions and FIFO trade rows.
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace pineforge;
using Phase = NativePathPhase;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr int touch = 7;

enum class Shape {
    First, Second, Third, Three, Stops, Split, SamePrice, OpenExit, Refill,
    SamePriceChildren, Mixed, LimitAndStop, Waypoint, TickTouch, TickMiss
};

struct Run {
    Shape shape;
    bool buy;
    // Relative to the older entry. Reflect the whole path for a short.
    double price(double d) const { return 64 + (buy ? d : -d); }
    Phase up() const { return buy ? Phase::High : Phase::Low; }
    Phase down() const { return buy ? Phase::Low : Phase::High; }
    Bar bar(int i, double o, double h, double l, double c) const {
        return {price(o), price(buy ? h : l), price(buy ? l : h), price(c),
                1000, (i + 1) * 60'000LL};
    }
    bool adds_if_partial() const {
        return shape == Shape::First || shape == Shape::Second || shape == Shape::Third;
    }
    std::vector<Bar> bars() const {
        std::vector<Bar> out;
        for (int i = 0; i < touch; ++i) {
            const int d = i == 3 && shape != Shape::SamePriceChildren ? -4
                        : i == 5 && shape == Shape::Three ? -2 : 0;
            out.push_back(bar(i, d, d, d, d));
        }
        switch (shape) {
        case Shape::First: case Shape::OpenExit: case Shape::Refill:
            out.push_back(bar(touch, 0, 10, -18, 0)); break; // O -> H first
        case Shape::Third:
            out.push_back(bar(touch, 7, 12, -6, 10)); break; // O -> H -> L -> C
        case Shape::Split:
            out.push_back(bar(touch, 0, 5, -10, 0)); break;
        case Shape::Stops: case Shape::SamePrice:
            out.push_back(bar(touch, 0, 2, -14, -6)); break;
        case Shape::TickTouch: case Shape::TickMiss:
            out.push_back(bar(touch, 0, shape == Shape::TickTouch ? 7.75 : 7.25, -2, 3));
            break;
        default:
            out.push_back(bar(touch, 0, 10, -2, 3)); break; // L -> H second
        }
        out.push_back(bar(touch + 1, 0, 0, 0, 0)); // also expose any queued add
        return out;
    }
};

// A body: destination phase (None for the ordinary close), open lots, closed rows.
using Body = std::tuple<Phase, std::size_t, int>;
// A fill: destination phase, order id, booked price. Includes opening fills.
using Fill = std::tuple<Phase, std::string, double>;
// A closed row: FIFO entry bar and price, exit id and price, quantity.
using Row = std::tuple<int, double, std::string, double, double>;

class Host final : public source::PineStrategyHost {
public:
    explicit Host(Run run) : r(run) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig c;
        c.calc_on_order_fills = true;
        c.pyramiding = r.shape == Shape::Three ? 3 : 2;
        configure_pine_strategy(c);
        set_syminfo_mintick(1);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
    }

    void on_source_bar(const Bar&) override {
        const auto point = current_execution_point().value();
        const bool fill = point.quote_kind == NativeCurrentQuoteKind::ExecutionAnchor;
        const int i = pine_bar_index();
        const auto lots = physical_position().lot_count;
        if (!fill && i < touch) {
            if (r.shape == Shape::LimitAndStop) {
                if (i == touch - 1) {
                    // The passive limit opens one side; the other entry's
                    // stop reverses it farther along the very same leg.
                    strategy_entry("E", !r.buy, r.price(4), na, 1);
                    strategy_entry("B", r.buy, na, r.price(8), 2);
                }
                return;
            }
            if (r.shape == Shape::OpenExit || r.shape == Shape::Refill) {
                if (i == touch - 1) entry(1);
                return;
            }
            if (i == 0) entry(r.shape == Shape::SamePrice ? 2 : 1);
            if (i == 2 && r.shape != Shape::Third && r.shape != Shape::Mixed
                && r.shape != Shape::SamePrice) entry(2);
            if (i == 4 && r.shape == Shape::Three) entry(1);
            if (i != touch - 1) return;
            if (r.shape == Shape::Third) {
                // The other opening fills on the way to the second extreme.
                // Its callback places both targets ahead of the return to C.
                strategy_entry("E", r.buy, r.price(-4), na, 2);
            } else if (r.shape == Shape::SamePrice) {
                for (const char* id : {"X1", "X2"})
                    strategy_exit(id, "E", na, r.price(-8), na, na, na, 100, "", 1);
            } else {
                bracket();
                if (r.shape == Shape::Mixed)
                    strategy_entry("B", r.buy, na, r.price(4), 1);
            }
        }
        if (i != touch) return;
        bodies.emplace_back(fill ? point.decision.coordinate.path_phase : Phase::None,
                            lots, trade_count());
        if (!fill) return;
        if (r.shape == Shape::OpenExit && lots == 1 && trade_count() == 0) bracket();
        if (r.shape == Shape::Refill && lots == 1 && trade_count() == 0)
            strategy_entry("second", r.buy, na, na, 1);
        if (r.shape == Shape::Third && lots == 2 && trade_count() == 0) bracket();
        if (!asked && ((r.adds_if_partial() && lots == 1 && trade_count() > 0)
                     || (r.shape == Shape::Waypoint && lots == 0 && trade_count() == 3))) {
            asked = true;
            strategy_entry(r.adds_if_partial() ? "E" : "R", r.buy, na, na, 1);
        }
    }

    std::vector<Body> bodies;
    std::vector<Fill> fills;
    bool asked = false;

private:
    void entry(double qty) { strategy_entry("E", r.buy, na, na, qty); }
    void bracket() {
        strategy_exit("X", "E", na, na, na, na, na, 100, "", na, "",
                      r.shape == Shape::Stops ? na : 8,
                      r.shape == Shape::Stops || r.shape == Shape::Split ? 8 : na);
    }
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& e,
                                 const NativeDecisionContext& ctx) override {
        if (ctx.coordinate.interval_index == touch)
            fills.emplace_back(ctx.coordinate.path_phase, e.request().label, e.resolved_price);
    }
    Run r;
};

bool check(const char* name, Run r) {
    const auto bars = r.bars();
    Host host(r);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");

    std::vector<Body> bodies;
    std::vector<Fill> fills;
    std::vector<Row> rows;
    std::size_t left = 0;
    const auto fill = [&](Phase phase, const char* id, int d) {
        fills.emplace_back(phase, id, r.price(d));
    };
    const auto row = [&](int i, int entry, const char* id, int exit, int qty) {
        rows.emplace_back(i, r.price(entry), id, r.price(exit), qty);
    };
    switch (r.shape) {
    case Shape::OpenExit:
        fill(Phase::Open, "E", 0); bodies.emplace_back(Phase::Open, 1, 0);
        fill(r.up(), "X", 8); row(touch, 0, "X", 8, 1);
        bodies.emplace_back(r.up(), 0, 1);
        break;
    case Shape::Refill:
        fill(Phase::Open, "E", 0); fill(Phase::Open, "second", 0);
        bodies = {{Phase::Open, 1, 0}, {Phase::Open, 2, 0}};
        left = 2;
        break;
    case Shape::SamePrice:
        fill(r.down(), "X1", -8); fill(r.down(), "X2", -8);
        row(1, 0, "X1", -8, 1); row(1, 0, "X2", -8, 1);
        bodies.emplace_back(r.down(), 0, 2);
        break;
    case Shape::Stops:
        fill(r.down(), "X", -8); fill(r.down(), "X", -12);
        row(1, 0, "X", -8, 1); row(3, -4, "X", -12, 2);
        bodies.emplace_back(r.down(), 0, 2);
        break;
    case Shape::SamePriceChildren:
        fill(r.up(), "X", 8); fill(r.up(), "X", 8);
        row(1, 0, "X", 8, 1); row(3, 0, "X", 8, 2);
        bodies.emplace_back(r.up(), 0, 2);
        break;
    case Shape::Mixed:
        fill(r.up(), "B", 4); fill(r.up(), "X", 8);
        bodies = {{r.up(), 2, 0}, {r.up(), 1, 1}};
        row(1, 0, "X", 8, 1);
        left = 1;
        break;
    case Shape::LimitAndStop:
        fill(r.up(), "E", 4); fill(r.up(), "B", 8);
        bodies = {{r.up(), 1, 0}, {r.up(), 1, 1}};
        row(touch, 4, "B", 8, 1);
        left = 1;
        break;
    default: {
        // The newer two-unit bracket spends one older and one newer unit
        // at its own target. There are two FIFO rows but only one fill here.
        const bool third = r.shape == Shape::Third;
        const Phase phase = third ? Phase::Close : r.up();
        const int newer = third ? touch : 3;
        if (third) {
            fill(r.down(), "E", -4);
            bodies.emplace_back(r.down(), 2, 0);
        }
        fill(phase, "X", 4);
        row(1, 0, "X", 4, 1); row(newer, -4, "X", 4, 1);
        if (r.shape == Shape::TickMiss) {
            left = 1; // one unit of the newer FIFO lot still has its exit
            bodies.emplace_back(phase, left, 2);
        } else if (r.shape == Shape::Split) {
            bodies.emplace_back(phase, 1, 2);
            fill(r.down(), "X", -8); row(newer, -4, "X", -8, 1);
            bodies.emplace_back(r.down(), 0, 3);
        } else {
            if (r.shape == Shape::Three) {
                fill(phase, "X", 6); row(newer, -4, "X", 6, 1);
            }
            fill(phase, "X", 8);
            if (r.shape == Shape::Three) row(5, -2, "X", 8, 1);
            else row(newer, -4, "X", 8, 1);
            bodies.emplace_back(phase, 0, rows.size());
            if (r.shape == Shape::Waypoint) {
                fill(phase, "R", 10);
                bodies.emplace_back(phase, 1, rows.size());
                left = 1;
            }
        }
        break;
    }
    }
    bodies.emplace_back(Phase::None, left, rows.size()); // ordinary close
    const auto position = host.physical_position();
    bool ok = host.last_error().empty() && host.fills == fills && host.bodies == bodies
        && host.asked == (r.shape == Shape::Waypoint) && position.lot_count == left
        && host.trade_count() == static_cast<int>(rows.size())
        && host.report_trade_count() == static_cast<int>(rows.size() + left);
    for (int i = 0; i < std::min(host.trade_count(), static_cast<int>(rows.size())); ++i) {
        const auto trade = host.get_trade(i);
        const auto& [bar, basis, id, target, qty] = rows[i];
        ok &= trade.entry_id == "E" && trade.entry_bar_index == bar && trade.entry_price == basis
            && trade.exit_id == id && trade.exit_bar_index == touch && trade.exit_price == target
            && trade.qty == qty && trade.is_long == (r.shape == Shape::LimitAndStop ? !r.buy : r.buy);
    }
    if (left) {
        const char* id = "E";
        int offset = 0;
        int entered = touch;
        if (r.shape == Shape::TickMiss) { offset = -4; entered = 3; }
        if (r.shape == Shape::Mixed) { id = "B"; offset = 4; }
        if (r.shape == Shape::LimitAndStop) { id = "B"; offset = 8; }
        if (r.shape == Shape::Waypoint) { id = "R"; offset = 10; }
        const auto lots = host.native_open_lots(bars.back().close);
        ok &= lots.size() == left;
        for (const auto& lot : lots) {
            const char* label = r.shape == Shape::Refill && lot.ordinal == 1 ? "second" : id;
            ok &= lot.entry_label == label && lot.entry_price == r.price(offset)
                && lot.entry_bar_index == entered && lot.signed_units == (r.buy ? 1 : -1);
        }
    }
    std::printf("%s %s %s: executions=%zu/%zu fills=%zu/%zu closed=%d/%zu open=%zu/%zu\n",
                ok ? "PASS" : "FAIL", r.buy ? "long" : "short", name, host.bodies.size(),
                bodies.size(), host.fills.size(), fills.size(), host.trade_count(), rows.size(),
                position.lot_count, left);
    if (!ok) {
        for (const auto& [phase, lots, closed] : host.bodies)
            std::printf("  body phase=%d open=%zu closed=%d\n", static_cast<int>(phase), lots, closed);
        for (const auto& [phase, id, price] : host.fills)
            std::printf("  fill phase=%d %s @%g\n", static_cast<int>(phase), id.c_str(), price);
        if (!host.last_error().empty()) std::printf("  %s\n", host.last_error().c_str());
    }
    return ok;
}
} // namespace

int main() {
    bool ok = true;
    for (bool buy : {true, false}) {
        ok &= check("first-leg", {Shape::First, buy});
        ok &= check("second-leg", {Shape::Second, buy});
        ok &= check("last-leg", {Shape::Third, buy});
        ok &= check("three-limits", {Shape::Three, buy});
        ok &= check("two-stops", {Shape::Stops, buy});
        ok &= check("different-legs", {Shape::Split, buy});
        ok &= check("shared-stop", {Shape::SamePrice, buy});
        ok &= check("equal-targets", {Shape::SamePriceChildren, buy});
        ok &= check("stop-entry-and-limit-exit", {Shape::Mixed, buy});
        ok &= check("limit-and-stop-entries", {Shape::LimitAndStop, buy});
        ok &= check("open-then-exit", {Shape::OpenExit, buy});
        ok &= check("open-refill", {Shape::Refill, buy});
        ok &= check("order-after-exits", {Shape::Waypoint, buy});
        ok &= check("tick-reaches-target", {Shape::TickTouch, buy});
        ok &= check("tick-misses-target", {Shape::TickMiss, buy});
    }
    return ok ? 0 : 1;
}
