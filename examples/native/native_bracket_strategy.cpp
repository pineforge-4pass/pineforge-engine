// Pine-free native example: a market entry with two anchored bracket legs on
// an instrument price grid (R5 lane L7b).
//
// The legs are placed BEFORE the entry has a price. Each carries a
// FromOwnerFill anchor spelled in ticks; when the entry's fill arms them the
// kernel materializes the level as fill + offset, snapped onto the run's tick
// ladder with NativeAnchorRounding::Directional (a sell limit up, a sell stop
// down), offers it once to resolve_anchored_level, and writes the installed
// level into the ArmedEvent. PendingUntilArmed keeps the legs out of the
// working enumeration until that arm, exactly like a broker that shows no
// working child before the parent fills.
//
//   c++ -std=c++17 native_bracket_strategy.cpp -lpineforge -o native_bracket
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.

#include <pineforge/native_toolkit.hpp>

#include <cstdio>
#include <iostream>
#include <optional>
#include <variant>

namespace {

namespace no = pineforge::native_order;
namespace tk = pineforge::native_toolkit;

class BracketExample : public pineforge::NativeStrategyHost {
    int bars_ = 0;
    no::RequestHandle entry_;
    tk::BracketReceipt legs_;

    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ != 1) return;
        // The entry: two units at the next open.
        const auto placed = submit({no::Transact{2.0}, "entry", "bracket"});
        if (placed.status != no::SubmitStatus::Accepted || !placed.handle) return;
        entry_ = *placed.handle;

        // The legs: relative to the fill, in ticks, level unknown until then.
        no::Request take_profit{no::Reduce{no::OwnerOpenedUnits{}}, "take-profit", "bracket"};
        take_profit.trigger = no::Limit{0.0};
        take_profit.anchor = no::FromOwnerFill{+12.0, /*ticks=*/true};
        no::Request stop_loss{no::Reduce{no::OwnerOpenedUnits{}}, "stop-loss", "bracket"};
        stop_loss.trigger = no::Stop{0.0};
        stop_loss.anchor = no::FromOwnerFill{-8.0, /*ticks=*/true};

        tk::BracketSpec bracket;
        bracket.parent = entry_;
        bracket.take_profit = take_profit;
        bracket.stop_loss = stop_loss;
        bracket.anchor_rounding = no::NativeAnchorRounding::Directional;
        bracket.visibility = no::NativeArmVisibility::PendingUntilArmed;
        legs_ = tk::submit_bracket(*this, bracket);

        // Before the fill the legs are live but not working orders: only the
        // entry is enumerated.
        std::cout << "working before the fill: " << native_working_requests().size() << '\n';
    }

    // The one policy point: the kernel level is offered once per leg. This
    // host keeps it (nullopt); a host with its own trigger projection would
    // return the level it wants installed instead.
    std::optional<double> resolve_anchored_level(
            const pineforge::NativeAnchoredLevelView& view) const override {
        std::printf("arm: %s leg of owner %llu, fill %.4f, offset %+.4f, kernel level %.4f\n",
                    view.trigger == pineforge::NativeAnchoredTrigger::Limit ? "limit" : "stop",
                    static_cast<unsigned long long>(view.owner.incarnation),
                    view.owner_fill_price, view.offset, view.kernel_level);
        return std::nullopt;
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const pineforge::NativeDecisionContext&) override {
        if (event.definition && event.definition->handle == entry_) {
            // The fill armed both legs: from here on they are working orders.
            std::cout << "working after the fill: " << native_working_requests().size() << '\n';
        }
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-bracket-example";
    spec.identity.run_number = 1;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    // A quarter-point ladder: the legs snap onto it at the arm.
    spec.price_tick = 0.25;
    spec.fee_kind = pineforge::NativeFeeKind::Percent;
    spec.fee_value = 0.0;
    return spec;
}

// open, high, low, close, volume, timestamp (Unix milliseconds). The entry
// fills at 100.10 (off the ladder); the legs arm at 103.25 (100.10 + 3.00
// rounded up onto the ladder) and 98.00 (100.10 - 2.00 rounded down), and the
// rise in the third bar takes the profit.
const pineforge::Bar kBars[] = {
    {100.00, 100.50,  99.75, 100.10, 4.0, 0},
    {100.10, 101.00, 100.00, 100.90, 4.0, 300000},
    {100.90, 103.60, 100.80, 103.20, 4.0, 600000},
    {103.20, 103.40, 102.90, 103.00, 4.0, 900000},
};
constexpr int kBarCount = 4;

}  // namespace

int main() {
    BracketExample host;
    if (host.configure_native(make_spec()).status
        != pineforge::NativeSetupStatus::Applied) {
        std::cerr << "configure: " << host.last_error() << '\n';
        return 1;
    }

    host.run(kBars, kBarCount);
    if (host.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "run: " << host.last_error() << '\n';
        return 1;
    }

    // The ArmedEvent carries the materialized level: read it back.
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        const auto* armed = std::get_if<no::ArmedEvent>(&*event.command);
        if (!armed || !armed->definition) continue;
        const auto& request = armed->definition->request;
        if (const auto* limit = std::get_if<no::Limit>(&request.trigger)) {
            std::cout << "armed " << request.label << " at " << limit->price << '\n';
        } else if (const auto* stop = std::get_if<no::Stop>(&request.trigger)) {
            std::cout << "armed " << request.label << " at " << stop->price << '\n';
        }
    }

    std::cout << "closed trades: " << host.trade_count() << '\n';
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto& trade = host.get_trade(i);
        std::cout << "  " << (trade.is_long ? "long " : "short")
                  << " qty=" << trade.qty
                  << " entry=" << trade.entry_price
                  << " exit=" << trade.exit_price
                  << " pnl=" << trade.pnl << '\n';
    }
    return host.trade_count() > 0 ? 0 : 1;
}
