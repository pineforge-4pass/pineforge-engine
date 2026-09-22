// Pine-free native example: a market entry with three anchored bracket legs
// -- take-profit, stop-loss and a trail -- on an instrument price grid (R5
// lane L7b).
//
// The legs are placed BEFORE the entry has a price. Each carries a
// FromOwnerFill anchor spelled in ticks; when the entry's fill arms them the
// kernel materializes the level as fill + offset, snapped onto the run's tick
// ladder with NativeAnchorRounding::Directional (a sell limit up, a sell stop
// down, and a trail's arm threshold up, like the limit it is reached as),
// offers it once to resolve_anchored_level, and writes the installed level
// into the ArmedEvent. PendingUntilArmed keeps the legs out of the working
// enumeration until that arm, exactly like a broker that shows no working
// child before the parent fills.
//
// The entry fills at 100.10, off the 0.25 ladder, so every level is snapped:
//
//   take-profit   100.10 + 12 ticks (3.00) = 103.10 -> up    -> 103.25
//   stop-loss     100.10 -  8 ticks (2.00) =  98.10 -> down  ->  98.00
//   trail arm     100.10 +  6 ticks (1.50) = 101.60 -> up    -> 101.75
//
// The working enumeration goes from 1 row (the entry) to 3 (the three armed
// legs) at the fill. The legs share one OCA group, so when the take-profit
// fills at 103.25 the kernel cancels the stop-loss and the trail with
// CancelReason::Group. The example asserts each of those facts.
//
//   c++ -std=c++17 native_bracket_strategy.cpp -lpineforge -o native_bracket
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.

#include <pineforge/native_toolkit.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <variant>

namespace {

namespace no = pineforge::native_order;
namespace tk = pineforge::native_toolkit;

const char* leg_name(tk::BracketLeg which) {
    switch (which) {
    case tk::BracketLeg::TakeProfit: return "take-profit";
    case tk::BracketLeg::StopLoss: return "stop-loss";
    case tk::BracketLeg::Trail: break;
    }
    return "trail";
}

const char* reject_name(no::RequestRejectReason reason) {
    switch (reason) {
    case no::RequestRejectReason::InvalidQuantity: return "InvalidQuantity";
    case no::RequestRejectReason::OffGrid: return "OffGrid";
    case no::RequestRejectReason::InvalidTrigger: return "InvalidTrigger";
    case no::RequestRejectReason::InvalidCapacity: return "InvalidCapacity";
    case no::RequestRejectReason::InvalidOwner: return "InvalidOwner";
    case no::RequestRejectReason::InvalidQuantityBasis: return "InvalidQuantityBasis";
    case no::RequestRejectReason::InvalidGroup: return "InvalidGroup";
    case no::RequestRejectReason::PlacementAdmission: break;
    }
    return "PlacementAdmission";
}

const char* action_name(tk::OrderBookAction action) {
    switch (action) {
    case tk::OrderBookAction::Replaced: return "replaced in place";
    case tk::OrderBookAction::ReplaceRejected: return "replacement REFUSED";
    case tk::OrderBookAction::SubmitAccepted: return "submitted fresh";
    case tk::OrderBookAction::SubmitRejected: break;
    }
    return "submit REFUSED";
}

// The same reading a receipt asks for, one key at a time. A returned handle
// alone cannot say whether the book re-priced the order the host named or
// replaced it with a brand new request born behind everything already
// resting, and an empty one cannot say whether the replacement was refused
// (the previous order still working) or the submit was (the key now unbound).
void report_key(const char* key, const tk::OrderBookOutcome& outcome) {
    std::printf("key %s: %s", key, action_name(outcome.action));
    if (const auto handle = outcome.handle()) {
        std::printf(", request %llu", static_cast<unsigned long long>(handle->incarnation));
    }
    if (const auto reason = outcome.reason()) {
        std::printf(" (%s)", reject_name(*reason));
    }
    std::printf(", ordinal %llu\n", static_cast<unsigned long long>(outcome.event_ordinal()));
}

// What a host must do with a receipt: read every leg, not just the handles.
// An empty handle alone cannot say whether the leg was never asked for or
// whether the kernel refused it -- and a refused exit leg that goes unread is
// a position left running without the protection its strategy believes it
// placed.
void report_legs(const tk::BracketReceipt& legs) {
    for (const tk::BracketLeg which : {tk::BracketLeg::TakeProfit, tk::BracketLeg::StopLoss,
                                       tk::BracketLeg::Trail}) {
        const tk::BracketLegOutcome& leg = legs.outcome(which);
        switch (leg.state) {
        case tk::BracketLegState::NotRequested:
            std::printf("leg %s: not requested\n", leg_name(which));
            break;
        case tk::BracketLegState::NotSubmitted:
            std::printf("leg %s: not submitted (the parent was never allocated)\n",
                        leg_name(which));
            break;
        case tk::BracketLegState::Accepted:
            std::printf("leg %s: accepted as request %llu\n", leg_name(which),
                        static_cast<unsigned long long>(leg.handle()->incarnation));
            break;
        case tk::BracketLegState::Rejected:
            std::printf("leg %s: REFUSED (%s)\n", leg_name(which),
                        leg.reason() ? reject_name(*leg.reason()) : "no reason given");
            break;
        }
    }
}

class BracketExample : public pineforge::NativeStrategyHost {
public:
    // Read back by main(). resolve_anchored_level is const, so the levels it
    // is offered are recorded in mutable members.
    std::size_t working_before_fill = 0;
    std::size_t working_after_fill = 0;
    mutable std::optional<double> offered_take_profit;
    mutable std::optional<double> offered_stop_loss;
    mutable std::optional<double> offered_trail_arm;

private:
    int bars_ = 0;
    no::RequestHandle entry_;
    tk::BracketReceipt legs_;
    std::optional<tk::OrderBook<std::string>> book_;

    void on_native_run_begin() override {
        bars_ = 0;
        book_.emplace(*this);
    }

    // A resting bid far under the market: it never fills, so it is only ever
    // the book's own order to re-price, refuse and withdraw.
    static no::Request ladder(double price, const char* label) {
        no::Request request{no::Transact{1.0}, label, "ladder"};
        request.trigger = no::Limit{price};
        return request;
    }

    // The key the host addresses that order by. Everything below reads the
    // outcome, never the bare handle.
    void work_the_book() {
        switch (bars_) {
        case 2: {
            const auto opened = book_->submit_or_replace_outcome("ladder", ladder(90.0, "bid-1"));
            report_key("ladder", opened);
            assert(opened.action == tk::OrderBookAction::SubmitAccepted);
            assert(opened.bound() && !opened.replace.has_value());
            break;
        }
        case 3: {
            // Re-pricing: the replace decided it and no submit happened, so
            // the host knows it kept its order rather than queueing a new one.
            const auto repriced =
                    book_->submit_or_replace_outcome("ladder", ladder(91.0, "bid-2"));
            report_key("ladder", repriced);
            assert(repriced.replaced());
            assert(repriced.replace && !repriced.submit.has_value());

            // A replacement the kernel must refuse: a reduce of zero units.
            // The empty handle alone would be indistinguishable from a
            // refused submit, and would carry no reason; the outcome says
            // both, and the order the host named is still working.
            no::Request zero{no::Reduce{no::ExplicitUnits{0.0}}, "bid-bad", "ladder"};
            zero.trigger = no::Limit{91.0};
            const auto refused = book_->submit_or_replace_outcome("ladder", zero);
            report_key("ladder", refused);
            assert(refused.action == tk::OrderBookAction::ReplaceRejected);
            assert(refused.reason() == no::RequestRejectReason::InvalidQuantity);
            assert(!refused.handle().has_value() && !refused.submit.has_value());
            assert(book_->handle("ladder") == repriced.handle());
            break;
        }
        case 4: {
            const auto withdrawn = book_->cancel_outcome("ladder");
            std::printf("key ladder: cancel commanded=%d status=%s\n",
                        withdrawn.commanded() ? 1 : 0,
                        withdrawn.status() == no::CancelStatus::Cancelled ? "Cancelled"
                                                                         : "NotWorking");
            assert(withdrawn.known && withdrawn.commanded());
            assert(withdrawn.status() == no::CancelStatus::Cancelled);
            // An unknown key is the book's own silence, not the kernel's.
            const auto never = book_->cancel_outcome("no-such-key");
            assert(!never.known && !never.commanded());
            assert(never.status() == no::CancelStatus::NotWorking);
            break;
        }
        default:
            break;
        }
    }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ != 1) {
            work_the_book();
            return;
        }
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
        // The trail rides 4 ticks behind its best once the price has reached
        // its arm threshold. The threshold is the anchored level, so arm_price
        // stays absent: the fill supplies it.
        no::Trail ride;
        ride.ticks = no::TrailTicks{4.0};
        no::Request trail{no::Reduce{no::OwnerOpenedUnits{}}, "trail", "bracket"};
        trail.trigger = ride;
        trail.anchor = no::FromOwnerFill{+6.0, /*ticks=*/true};

        tk::BracketSpec bracket;
        bracket.parent = entry_;
        bracket.take_profit = take_profit;
        bracket.stop_loss = stop_loss;
        bracket.trail = trail;
        bracket.anchor_rounding = no::NativeAnchorRounding::Directional;
        bracket.visibility = no::NativeArmVisibility::PendingUntilArmed;
        legs_ = tk::submit_bracket(*this, bracket);

        // The receipt, leg by leg. A host that reads only the handles cannot
        // tell a refused leg from one it never asked for, which is how an
        // anchored leg can vanish while the strategy keeps running: assert
        // instead that every leg this bracket asked for is resting.
        report_legs(legs_);
        assert(legs_.every_requested_leg_accepted());
        assert(legs_.outcome(tk::BracketLeg::TakeProfit).state
               == tk::BracketLegState::Accepted);
        assert(legs_.outcome(tk::BracketLeg::StopLoss).state == tk::BracketLegState::Accepted);
        assert(legs_.outcome(tk::BracketLeg::TakeProfit).handle() == legs_.take_profit);
        assert(!legs_.outcome(tk::BracketLeg::TakeProfit).reason().has_value());
        // expectation corrected: trail NotRequested -> Accepted, because the
        // bracket now asks for its trailing leg (R5 lane F12: the example
        // arms a take-profit, a stop-loss AND a trail). A leg the spec leaves
        // unset still reads NotRequested, with no result, never a refusal.
        assert(legs_.outcome(tk::BracketLeg::Trail).state == tk::BracketLegState::Accepted);
        assert(legs_.outcome(tk::BracketLeg::Trail).result.has_value());
        assert(legs_.trail.has_value() && legs_.outcome(tk::BracketLeg::Trail).handle() == legs_.trail);

        // Before the fill the legs are live but not working orders: only the
        // entry is enumerated.
        working_before_fill = native_working_requests().size();
        std::cout << "working before the fill: " << working_before_fill << '\n';
        assert(working_before_fill == 1);
    }

    // The one policy point: the kernel level is offered once per leg. This
    // host keeps it (nullopt); a host with its own trigger projection would
    // return the level it wants installed instead.
    std::optional<double> resolve_anchored_level(
            const pineforge::NativeAnchoredLevelView& view) const override {
        const char* trigger = "stop";
        if (view.trigger == pineforge::NativeAnchoredTrigger::Limit) {
            trigger = "limit";
            offered_take_profit = view.kernel_level;
        } else if (view.trigger == pineforge::NativeAnchoredTrigger::TrailArm) {
            trigger = "trail arm";
            offered_trail_arm = view.kernel_level;
        } else {
            offered_stop_loss = view.kernel_level;
        }
        std::printf("arm: %s leg of owner %llu, fill %.4f, offset %+.4f, kernel level %.4f\n",
                    trigger, static_cast<unsigned long long>(view.owner.incarnation),
                    view.owner_fill_price, view.offset, view.kernel_level);
        return std::nullopt;
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const pineforge::NativeDecisionContext&) override {
        if (event.definition && event.definition->handle == entry_) {
            // The fill armed all three legs: from here on they are working
            // orders, and the entry itself is done.
            working_after_fill = native_working_requests().size();
            std::cout << "working after the fill: " << working_after_fill << '\n';
            assert(working_after_fill == 3);
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
// rounded up onto the ladder), 98.00 (100.10 - 2.00 rounded down) and, for
// the trail, 101.75 (100.10 + 1.50 rounded up), and the rise in the third bar
// takes the profit: its path dips to 100.80 first, then climbs through the
// trail's arm threshold to the take-profit's 103.25.
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

    // The ArmedEvent carries the materialized level: read it back, and the
    // cancellations the take-profit's fill caused.
    std::optional<double> take_profit_level;
    std::optional<double> stop_loss_level;
    std::optional<double> trail_arm_level;
    int group_cancels = 0;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (const auto* cancelled = std::get_if<no::CancelledEvent>(&*event.command)) {
            // The one-cancels-all group: whichever leg fills first ends its
            // siblings, and the event names the group as the reason.
            const std::string& label = cancelled->request().label;
            if (label == "stop-loss" || label == "trail") {
                std::cout << "cancelled " << label << " (reason "
                          << (cancelled->reason == no::CancelReason::Group ? "Group" : "other")
                          << ")\n";
                assert(cancelled->reason == no::CancelReason::Group);
                ++group_cancels;
            }
            continue;
        }
        const auto* armed = std::get_if<no::ArmedEvent>(&*event.command);
        if (!armed || !armed->definition) continue;
        const auto& request = armed->definition->request;
        if (const auto* limit = std::get_if<no::Limit>(&request.trigger)) {
            std::cout << "armed " << request.label << " at " << limit->price << '\n';
            take_profit_level = limit->price;
        } else if (const auto* stop = std::get_if<no::Stop>(&request.trigger)) {
            std::cout << "armed " << request.label << " at " << stop->price << '\n';
            stop_loss_level = stop->price;
        } else if (const auto* trail = std::get_if<no::Trail>(&request.trigger)) {
            // The anchored arm threshold is written into the armed definition;
            // the tick-spelled ride is resolved into a price distance.
            std::cout << "armed " << request.label << " at "
                      << trail->arm_price.value_or(-1.0) << ", riding " << trail->offset
                      << " behind its best\n";
            trail_arm_level = trail->arm_price;
            assert(trail->offset == 1.0);
        }
    }

    // Every level is the one the hook was offered, and every one sits on the
    // 0.25 ladder, snapped toward the region its leg needs.
    const auto on_ladder = [](double level) {
        const double ticks = level / 0.25;
        return ticks == std::floor(ticks);
    };
    assert(take_profit_level == 103.25 && host.offered_take_profit == take_profit_level);
    assert(stop_loss_level == 98.00 && host.offered_stop_loss == stop_loss_level);
    assert(trail_arm_level == 101.75 && host.offered_trail_arm == trail_arm_level);
    assert(on_ladder(*take_profit_level) && on_ladder(*stop_loss_level)
           && on_ladder(*trail_arm_level));
    // Working rows went 1 -> 3 at the fill, and the take-profit's fill
    // cancelled both of its siblings.
    assert(host.working_before_fill == 1 && host.working_after_fill == 3);
    assert(group_cancels == 2);
    assert(host.trade_count() == 1 && host.get_trade(0).exit_id == "take-profit"
           && host.get_trade(0).exit_price == 103.25);

    // The summary line, printed only once every check above has passed.
    std::cout << "armed tp=" << *take_profit_level << " sl=" << *stop_loss_level
              << " trail=" << *trail_arm_level << "  working " << host.working_before_fill
              << " -> " << host.working_after_fill << "  oca cancels: " << group_cancels
              << "  closed trades: " << host.trade_count() << '\n';
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
