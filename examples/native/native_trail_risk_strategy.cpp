// Pine-free native example: a trail spelled in ticks, the working book, and a
// generic account risk limit (R5 lanes L7 and L9).
//
// L7, order ergonomics:
//   * Trail{offset, arm_price, ticks = TrailTicks{8}}: the stop rides eight
//     ticks (8 x 0.25 = 2.00) behind the running best once the arm price is
//     reached; acceptance resolves the tick spelling into the price offset
//     once, so the stored definition carries a plain distance;
//   * trail_state(handle): the live trail's best price and current level;
//   * native_working_requests(): the working book, copied out;
//   * cancel_where(comment): bulk cancellation by comment.
// L9, risk limits: NativeRunSpec::risk with max_fills_per_day = 2 and
// action BlockOpenings. The entry and the trail exit are the day's two fills;
// the re-entry submitted right after the exit is ACCEPTED at submit and then
// refused at its matching candidate (MatchRejectReason::RiskLimit) while the
// block lasts, so it never reaches the book. native_risk_state() reads the
// ledger back and the NativeRiskEvent names the limit that tripped.
//
//   c++ -std=c++17 native_trail_risk_strategy.cpp -lpineforge_kernel -o trail_risk
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.

#include <pineforge/native_host.hpp>

#include <cstdio>
#include <iostream>
#include <optional>
#include <variant>
#include <vector>

namespace {

namespace no = pineforge::native_order;

class TrailRiskExample : public pineforge::NativeStrategyHost {
public:
    std::optional<pineforge::NativeTrailState> armed_state;
    std::optional<pineforge::NativeTrailState> last_state;
    std::optional<double> stored_trail_offset;   // after acceptance resolved the ticks
    std::size_t working_after_trail = 0;
    std::size_t cancelled_by_comment = 0;
    std::optional<no::RequestHandle> trail;
    std::optional<no::RequestHandle> re_entry;
    bool re_entry_accepted_at_submit = false;

private:
    int bars_ = 0;
    std::optional<no::RequestHandle> entry_;

    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1) {
            entry_ = submit({no::Transact{2.0}, "entry", "trail-risk"}).handle;
            // A resting buy far below the market, cancelled by comment later.
            no::Request far{no::Transact{1.0}, "far-limit", "parked"};
            far.trigger = no::Limit{90.0};
            submit(far);
            return;
        }
        if (bars_ == 2) {
            // The trail: arm at 102.00, then ride 8 ticks behind the best.
            no::Request request{no::Reduce{no::ExplicitUnits{2.0}}, "trail", "trail-risk"};
            no::Trail spelled;
            spelled.offset = 0.0;
            spelled.arm_price = 102.0;
            spelled.ticks = no::TrailTicks{8.0};
            request.trigger = spelled;
            trail = submit(request).handle;
            // The working book, copied out: the stored trail carries the
            // resolved price offset, and the ticks spelling is gone.
            const auto working = native_working_requests();
            working_after_trail = working.size();
            for (const auto& row : working) {
                if (trail && row.definition->handle == *trail) {
                    if (const auto* live = std::get_if<no::Trail>(&row.definition->request.trigger)) {
                        stored_trail_offset = live->offset;
                    }
                }
            }
            return;
        }
        if (trail) {
            if (const auto state = trail_state(*trail); state && state->activated) {
                if (!armed_state) armed_state = state;
                last_state = state;
            }
        }
        if (bars_ == 6) cancelled_by_comment = cancel_where("parked");
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const pineforge::NativeDecisionContext&) override {
        if (trail && event.handle() == *trail) {
            // The day's fill budget is spent: this opening is accepted at
            // submit and refused at every candidate while the block lasts.
            const auto placed = submit({no::Transact{1.0}, "re-entry", "trail-risk"});
            re_entry_accepted_at_submit = placed.status == no::SubmitStatus::Accepted;
            re_entry = placed.handle;
        }
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-trail-risk-example";
    spec.identity.run_number = 1;
    // This example reads its whole event record once the run has ended, so
    // it keeps it all: the default, Window, keeps only what a host has not
    // yet acknowledged (native_acknowledge_events).
    spec.event_retention = pineforge::NativeEventRetention::Full;
    spec.input_tf = "15";
    spec.script_tf = "15";
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
    spec.price_tick = 0.25;
    spec.fee_kind = pineforge::NativeFeeKind::Percent;
    spec.fee_value = 0.0;
    // The account's risk rule: two fills a day, then no more openings.
    pineforge::NativeRiskLimits risk;
    risk.max_fills_per_day = 2u;
    risk.action = pineforge::NativeRiskAction::BlockOpenings;
    spec.risk = risk;
    return spec;
}

constexpr std::int64_t kQuarter = 15LL * 60LL * 1000LL;

// open, high, low, close, volume, timestamp (Unix milliseconds). The entry
// fills at 100.00; the trail arms at 102.00 (best 103.00, level 101.00),
// follows the best to 106.00 (level 104.00) and exits when the fifth bar
// trades down through 104.00.
const pineforge::Bar kBars[] = {
    { 99.75, 100.25,  99.50, 100.00, 10.0, 0 * kQuarter},
    {100.00, 101.00,  99.75, 100.75, 10.0, 1 * kQuarter},
    {100.75, 103.00, 100.50, 102.75, 10.0, 2 * kQuarter},
    {102.75, 106.00, 102.50, 105.75, 10.0, 3 * kQuarter},
    {105.75, 105.75, 101.00, 101.50, 10.0, 4 * kQuarter},
    {101.50, 102.00, 101.00, 101.75, 10.0, 5 * kQuarter},
};
constexpr int kBarCount = 6;

const char* limit_name(no::RiskLimitKind kind) {
    switch (kind) {
        case no::RiskLimitKind::MaxDrawdown: return "MaxDrawdown";
        case no::RiskLimitKind::MaxIntradayLoss: return "MaxIntradayLoss";
        case no::RiskLimitKind::MaxConsecutiveLossDays: return "MaxConsecutiveLossDays";
        case no::RiskLimitKind::MaxFillsPerDay: return "MaxFillsPerDay";
    }
    return "?";
}

}  // namespace

int main() {
    TrailRiskExample host;
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

    // --- L7: the trail --------------------------------------------------------
    std::printf("working requests after the trail was placed: %zu; stored trail offset %.2f "
                "(8 ticks x 0.25, resolved at acceptance)\n",
                host.working_after_trail,
                host.stored_trail_offset ? *host.stored_trail_offset : -1.0);
    if (host.working_after_trail != 2 || !host.stored_trail_offset || *host.stored_trail_offset != 2.0) {
        std::cerr << "expected the far limit and the trail working, with the ticks resolved to 2.00\n";
        return 1;
    }
    if (!host.armed_state || !host.last_state) {
        std::cerr << "expected the trail to arm and track\n";
        return 1;
    }
    std::printf("trail armed: best=%.2f level=%.2f; last tracked: best=%.2f level=%.2f\n",
                host.armed_state->best_price, host.armed_state->current_level,
                host.last_state->best_price, host.last_state->current_level);
    if (host.armed_state->best_price != 103.0 || host.armed_state->current_level != 101.0
        || host.last_state->best_price != 106.0 || host.last_state->current_level != 104.0) {
        std::cerr << "expected the level to ride 2.00 behind the running best\n";
        return 1;
    }
    std::printf("cancel_where(\"parked\") cancelled %zu request(s)\n", host.cancelled_by_comment);
    if (host.cancelled_by_comment != 1) {
        std::cerr << "expected the far limit to be cancelled by its comment\n";
        return 1;
    }

    // --- L9: the risk limit -----------------------------------------------------
    int refused_on_risk = 0;
    std::optional<no::NativeRiskEvent> tripped;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (const auto* rejected = std::get_if<no::MatchRejectedEvent>(&*event.command)) {
            if (host.re_entry && rejected->handle() == *host.re_entry
                && rejected->reason == no::MatchRejectReason::RiskLimit) {
                ++refused_on_risk;
            }
        }
        if (const auto* risk = std::get_if<no::NativeRiskEvent>(&*event.command)) {
            if (!tripped) tripped = *risk;
        }
    }
    const auto ledger = host.native_risk_state();
    std::printf("risk ledger: blocked=%d fills_today=%llu reason=%s\n",
                ledger.blocked ? 1 : 0, static_cast<unsigned long long>(ledger.fills_today),
                ledger.reason ? limit_name(*ledger.reason) : "-");
    if (tripped) {
        std::printf("risk event: %s limit=%.0f observed=%.0f\n",
                    limit_name(tripped->kind), tripped->limit, tripped->observed);
    }
    std::printf("re-entry: %s at submit, refused on the risk block at %d candidate(s), final position %.2f\n",
                host.re_entry_accepted_at_submit ? "accepted" : "rejected", refused_on_risk,
                host.physical_position().signed_units);
    if (!ledger.blocked || ledger.fills_today != 2 || !ledger.reason
        || *ledger.reason != no::RiskLimitKind::MaxFillsPerDay || !tripped
        || !host.re_entry_accepted_at_submit || refused_on_risk == 0
        || host.physical_position().signed_units != 0.0) {
        std::cerr << "expected max_fills_per_day to block the re-entry after two fills\n";
        return 1;
    }

    std::cout << "closed trades: " << host.trade_count() << '\n';
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto& trade = host.get_trade(i);
        std::cout << "  " << (trade.is_long ? "long " : "short")
                  << " qty=" << trade.qty
                  << " entry=" << trade.entry_price
                  << " exit=" << trade.exit_price
                  << " pnl=" << trade.pnl
                  << " exit_id=" << trade.exit_id << '\n';
    }
    return host.trade_count() > 0 ? 0 : 1;
}
