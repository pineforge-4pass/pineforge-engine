// A percentage on a resting reversal is money at the fill. Growing the long
// book by that same *number of contracts* must not consume its short leg.
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = native_order;
using Phase = NativePathPhase;

constexpr double capital = 5225, point_value = 2, fx = 1.25, step = 0.25;
constexpr double held = 1, operand = 5;
constexpr double seed_price = 24, add_price = 40, stop_price = 8;
constexpr double na = std::numeric_limits<double>::quiet_NaN();

struct Row {
    const char* name;
    QtyType type;
    double amount, add;
};

// Book both long lots at their specified prices and realize them at the stop.
// No fees. Neither the book value nor the quotient comes from an engine query.
double expected_short(const Row& row) {
    if (row.type == QtyType::FIXED) {
        // Exact-exhaustion control: the fixed transaction pays for both closes.
        return (held + row.amount) - (held + row.add);
    }
    const double equity = capital + ((stop_price - seed_price) * held
        + (stop_price - add_price) * row.add) * point_value * fx;
    const double money = row.type == QtyType::CASH ? row.amount : equity * row.amount / 100;
    const double contracts = money / (stop_price * point_value * fx);
    return std::floor(contracts / step) * step;
}

constexpr Bar bar(int i, double o, double h, double l, double c) {
    return {o, h, l, c, 1000, 1'704'067'200'000LL + 60'000LL * i};
}
constexpr Bar flat(int i, double p) { return bar(i, p, p, p, p); }
constexpr std::array<Bar, 6> bars{{
    flat(0, seed_price), flat(1, seed_price), flat(2, seed_price),
    flat(3, add_price), bar(4, add_price, 41, 7, 10), flat(5, 10)
}};

struct Fill {
    std::string id;
    int bar;
    Phase phase;
    bool market, stop;
    double price, closed, opened;
    std::int64_t cycle_before, cycle_after;
};

const char* phase_name(Phase phase) {
    switch (phase) {
    case Phase::Open: return "Open";
    case Phase::High: return "High";
    case Phase::Low: return "Low";
    case Phase::Close: return "Close";
    default: return "None";
    }
}

class Probe final : public source::PineStrategyHost {
public:
    explicit Probe(const Row& row) : row_(row) {
        source::PineStrategyConfig config;
        config.initial_capital = capital;
        config.pyramiding = 3;
        config.margin_long = config.margin_short = 1;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_mintick(0.25);
        set_syminfo_pointvalue(point_value);
        set_syminfo_metadata("account_currency_fx", fx);
        set_syminfo_metadata("qty_step", step);
    }

    void on_source_bar(const Bar&) override {
        last_bar = pine_bar_index();
        switch (last_bar) {
        case 0:
            strategy_entry("Seed", true, na, na, held);
            break;
        case 1:
            at_placement = physical_position().signed_units;
            strategy_entry("Reverse", false, na, stop_price, row_.amount, "", "", 0,
                           static_cast<int>(row_.type));
            break;
        case 2:
            strategy_entry("Add", true, na, na, row_.add);
            break;
        case 3:
            before_trigger = physical_position().signed_units;
            break;
        }
    }

    void on_native_applied_event(const no::ExecutionAppliedEvent& e,
                                 const NativeDecisionContext&) override {
        fills.push_back({e.request().label, e.interval_index(), e.cursor.point.path_phase,
                         std::holds_alternative<no::Market>(e.request().trigger),
                         std::holds_alternative<no::Stop>(e.request().trigger),
                         e.resolved_price, e.closed_units, e.opened_units,
                         e.cycle_before, e.cycle_after});
    }

    int last_bar = -1;
    double at_placement = na, before_trigger = na;
    std::vector<Fill> fills;

private:
    const Row& row_;
};

const char* side(double signed_units) {
    return signed_units < 0 ? "SHORT" : signed_units > 0 ? "LONG" : "FLAT";
}

// 0: expected settlement; 1: the reported close-only result; 2: unusable witness.
// The control/witness distinction affects reporting only. All orders and
// settlement expectations use the same path.
int measure(const Row& row, bool witness = false) {
    Probe p(row);
    p.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    const auto invalid = [&](const char* why) {
        std::printf("INVALID %s: %s; last_bar=%d error='%s' at_placement=%g before_trigger=%g\n",
                    row.name, why, p.last_bar, p.last_error().c_str(), p.at_placement,
                    p.before_trigger);
        for (const auto& f : p.fills)
            std::printf("  %s bar=%d phase=%s price=%g closed=%g opened=%g\n",
                        f.id.c_str(), f.bar, phase_name(f.phase), f.price, f.closed, f.opened);
        return 2;
    };
    const double book = held + row.add;
    if (p.native_state().kind != NativeLifecycleKind::Completed || !p.last_error().empty()
        || p.last_bar != static_cast<int>(bars.size()) - 1)
        return invalid("run did not complete");
    if (p.fills.size() != 3) return invalid("expected exactly Seed, Add, Reverse fills");
    const auto& seed = p.fills[0];
    const auto& add = p.fills[1];
    const auto& rev = p.fills[2];
    if (seed.id != "Seed" || !seed.market || seed.bar != 1 || seed.phase != Phase::Open
        || seed.price != seed_price || seed.opened != held || seed.closed != 0
        || p.at_placement != held)
        return invalid("seed long missing at reversal placement");
    if (add.id != "Add" || !add.market || add.bar != 3 || add.phase != Phase::Open
        || add.price != add_price || add.opened != row.add || add.closed != 0
        || p.before_trigger != book)
        return invalid("same-side add did not establish the specified book");
    if (rev.id != "Reverse" || !rev.stop || rev.bar != 4 || rev.phase != Phase::Low
        || rev.price != stop_price || rev.closed != book)
        return invalid("stop did not fill and close both longs on the downward leg");
    if (seed.cycle_after <= 0 || seed.cycle_after != add.cycle_before
        || add.cycle_before != add.cycle_after || add.cycle_after != rev.cycle_before)
        return invalid("position cycle changed before reversal");

    const double units = expected_short(row);
    if (!std::isfinite(units) || (row.type == QtyType::FIXED ? units != 0 : units <= 0))
        return invalid("expected a fully consumed fixed transaction or tradable typed units");
    const double end = p.physical_position().signed_units;
    const bool matches = rev.opened == -units && end == -units;
    const bool false_close_only = witness && units > 0 && rev.opened == 0 && end == 0;
    std::printf("%s %s: Reverse bar=%d phase=%s price=%g closed=%g opened=%g end=%s(%g); "
                "expected opened=%g end=%s(%g)\n",
                matches ? "PASS" : false_close_only ? "FAIL" : "INVALID", row.name,
                rev.bar, phase_name(rev.phase), rev.price, rev.closed, rev.opened + 0.0,
                side(end), end + 0.0, 0.0 - units, side(-units), 0.0 - units);
    return matches ? 0 : false_close_only ? 1 : 2;
}
} // namespace

int main() {
    constexpr Row controls[]{
        {"percent/noncollision", QtyType::PERCENT_OF_EQUITY, operand, operand - step},
        {"percent/half-operand", QtyType::PERCENT_OF_EQUITY, operand / 2, operand - step},
        {"fixed/collision", QtyType::FIXED, operand, operand},
        {"cash/collision", QtyType::CASH, operand, operand},
    };
    bool controls_ok = true;
    for (const auto& row : controls)
        if (measure(row) != 0) controls_ok = false;
    if (!controls_ok) return 2;
    return measure({"percent/collision", QtyType::PERCENT_OF_EQUITY, operand, operand}, true);
}
