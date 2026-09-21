// Pine-free native example: account risk limits as the desk's stop-out rule
// (R5 lane L9). native_trail_risk_strategy.cpp shows the fill-count cap with
// BlockOpenings; this one shows the money limits, the kernel's own flatten and
// the risk day.
//
// The strategy has no stop of its own: once a day, at the day's first
// calculation, it buys 100 units at the market and holds. The ACCOUNT carries
// the rule, declared once in NativeRunSpec::risk:
//   * max_intraday_loss = 1 % of the day's opening equity;
//   * max_consecutive_loss_days = 2;
//   * max_drawdown = 5 % of the running peak (spelled, never reached here);
//   * action = FlattenAndBlock: at a breach the kernel closes the book with
//     one Flatten of its own (RequestOrigin::KernelRisk, ticket
//     "__kernel_risk__") at the point the breach was measured at, then refuses
//     every opening while the block lasts;
//   * day_basis = CalendarDayInTimezone: the civil date of the spec timezone.
//
// Three days of 6-hour bars:
//   day 1  long 100 @ 100.00; the third bar opens at 88.00: the marked equity
//          is 1200 under the day's opening 100000 (limit 1 % = 1000), so the
//          kernel flattens there (-1200) and blocks the day. The strategy's
//          same-day re-entry is accepted at submit and refused at its match.
//   day 2  a new day re-arms the intraday limit: long 100 @ 90.00; the third
//          bar opens at 80.00: 1000 under the day's opening 98800 (limit 988),
//          flattened (-1000), blocked.
//   day 3  its open settles day 2 as the second losing day in a row:
//          MaxConsecutiveLossDays blocks openings to the end of the run, and
//          the day-3 entry never fills.
//
//   c++ -std=c++17 native_risk_limits_strategy.cpp -lpineforge_kernel -o risk_limits
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.
// TradingView's strategy.risk.* rules are a different model and stay in the
// Pine adapter, which never declares this block; the generic limits are a
// native-host feature by ruling — docs/design/native-feature-parity.md §3.6.

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

namespace no = pineforge::native_order;

constexpr std::int64_t kSixHours = 6LL * 60LL * 60LL * 1000LL;
constexpr int kBarsPerDay = 4;
// 2025-01-06 00:00 UTC, a Monday: bar 0 opens a civil day.
constexpr std::int64_t kFirstOpen = 1736121600000LL;

class RiskLimitsExample : public pineforge::NativeStrategyHost {
public:
    std::optional<no::RequestHandle> same_day_re_entry;
    std::optional<no::RequestHandle> day_three_entry;
    // The ledger as the strategy saw it at each day's last calculation.
    std::vector<pineforge::NativeRiskState> ledger_at_day_end;

private:
    int bars_ = 0;

    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        const int index = bars_++;
        const int day = index / kBarsPerDay;
        const int bar_of_day = index % kBarsPerDay;
        if (bar_of_day == 0) {
            // The day's one entry. It fills at the next bar's open.
            const auto placed = submit({no::Transact{100.0}, "daily-long", "risk-limits"});
            if (day == 2) day_three_entry = placed.handle;
        }
        if (day == 0 && bar_of_day == 2) {
            // The kernel has just flattened the book under the intraday limit.
            // Trying again the same day is accepted here and refused at its
            // match: the block refuses openings, it does not reject commands.
            same_day_re_entry = submit({no::Transact{100.0}, "same-day-re-entry", "risk-limits"}).handle;
        }
        if (bar_of_day == kBarsPerDay - 1) ledger_at_day_end.push_back(native_risk_state());
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-risk-limits-example";
    spec.identity.run_number = 1;
    spec.input_tf = "360";
    spec.script_tf = "360";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = pineforge::NativeFeeKind::Percent;
    spec.fee_value = 0.0;

    pineforge::NativeRiskLimits risk;
    risk.max_intraday_loss = pineforge::NativeLossLimit{1.0, true};   // 1 % of the day's open
    risk.max_consecutive_loss_days = 2u;
    risk.max_drawdown = pineforge::NativeLossLimit{5.0, true};        // 5 % of the peak
    risk.day_basis = pineforge::NativeRiskDay::CalendarDayInTimezone;
    risk.action = pineforge::NativeRiskAction::FlattenAndBlock;
    spec.risk = risk;
    return spec;
}

pineforge::Bar bar(int index, double open, double high, double low, double close) {
    return {open, high, low, close, 10.0, kFirstOpen + index * kSixHours};
}

// Four 6-hour bars a day. The entry fills at the second bar's open, the third
// bar gaps down through the account's intraday limit, the fourth drifts.
std::vector<pineforge::Bar> make_tape() {
    return {
        bar(0, 100.00, 100.50,  99.50, 100.00),
        bar(1, 100.00, 100.25,  94.00,  95.00),   // long 100 @ 100.00; -500 at the close
        bar(2,  88.00,  89.00,  87.50,  88.50),   // opens 1200 under the day's open: flatten @ 88.00
        bar(3,  88.50,  90.50,  88.00,  90.00),

        bar(4,  90.00,  90.50,  89.50,  90.00),   // day 2: the intraday limit is re-armed
        bar(5,  90.00,  90.25,  86.00,  87.00),   // long 100 @ 90.00
        bar(6,  80.00,  81.00,  79.50,  80.50),   // opens 1000 under 98800 (limit 988): flatten @ 80.00
        bar(7,  80.50,  82.00,  80.00,  81.50),

        bar(8,  81.50,  82.00,  81.00,  81.75),   // day 3 opens: two losing days in a row
        bar(9,  81.75,  83.00,  81.50,  82.50),   // the day-3 entry is refused here
        bar(10, 82.50,  83.50,  82.00,  83.00),
        bar(11, 83.00,  84.00,  82.50,  83.50),
    };
}

const char* limit_name(no::RiskLimitKind kind) {
    switch (kind) {
        case no::RiskLimitKind::MaxDrawdown: return "MaxDrawdown";
        case no::RiskLimitKind::MaxIntradayLoss: return "MaxIntradayLoss";
        case no::RiskLimitKind::MaxConsecutiveLossDays: return "MaxConsecutiveLossDays";
        case no::RiskLimitKind::MaxFillsPerDay: return "MaxFillsPerDay";
    }
    return "?";
}

bool near(double actual, double expected) { return std::fabs(actual - expected) <= 1e-9; }

}  // namespace

int main() {
    RiskLimitsExample host;
    if (host.configure_native(make_spec()).status != pineforge::NativeSetupStatus::Applied) {
        std::cerr << "configure: " << host.last_error() << '\n';
        return 1;
    }
    const auto tape = make_tape();
    host.run(tape.data(), static_cast<int>(tape.size()));
    if (host.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "run: " << host.last_error() << '\n';
        return 1;
    }

    // --- the breaches, in the order the kernel reported them ---------------------
    std::vector<no::NativeRiskEvent> breaches;
    int kernel_flattens = 0;
    int re_entry_refusals = 0;
    int day_three_refusals = 0;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (const auto* risk = std::get_if<no::NativeRiskEvent>(&*event.command)) {
            breaches.push_back(*risk);
        }
        if (const auto* accepted = std::get_if<no::AcceptedEvent>(&*event.command)) {
            if (accepted->definition
                && accepted->definition->origin == no::RequestOrigin::KernelRisk) {
                ++kernel_flattens;
            }
        }
        if (const auto* rejected = std::get_if<no::MatchRejectedEvent>(&*event.command)) {
            if (rejected->reason != no::MatchRejectReason::RiskLimit) continue;
            if (host.same_day_re_entry && rejected->handle() == *host.same_day_re_entry) {
                ++re_entry_refusals;
            }
            if (host.day_three_entry && rejected->handle() == *host.day_three_entry) {
                ++day_three_refusals;
            }
        }
    }
    for (const auto& breach : breaches) {
        std::printf("risk event: %-22s limit=%8.2f observed=%8.2f at script bar %lld\n",
                    limit_name(breach.kind), breach.limit, breach.observed,
                    static_cast<long long>(breach.cursor.point.interval_index));
    }
    // Hand-computed: 1 % of 100000 against 100 x (100 - 88); 1 % of 98800
    // against 100 x (90 - 80); then the streak of two against its limit of two.
    if (breaches.size() != 3
        || breaches[0].kind != no::RiskLimitKind::MaxIntradayLoss
        || !near(breaches[0].limit, 1000.0) || !near(breaches[0].observed, 1200.0)
        || breaches[0].cursor.point.interval_index != 2
        || breaches[1].kind != no::RiskLimitKind::MaxIntradayLoss
        || !near(breaches[1].limit, 988.0) || !near(breaches[1].observed, 1000.0)
        || breaches[1].cursor.point.interval_index != 6
        || breaches[2].kind != no::RiskLimitKind::MaxConsecutiveLossDays
        || !near(breaches[2].limit, 2.0) || !near(breaches[2].observed, 2.0)
        || breaches[2].cursor.point.interval_index != 8) {
        std::cerr << "expected two intraday-loss breaches and the loss-days breach\n";
        return 1;
    }
    if (breaches[1].day_ordinal != breaches[0].day_ordinal + 1
        || breaches[2].day_ordinal != breaches[1].day_ordinal + 1) {
        std::cerr << "expected the three breaches on three consecutive risk days\n";
        return 1;
    }

    std::printf("kernel-originated flattens: %d; same-day re-entry refused %d time(s); "
                "day-3 entry refused %d time(s)\n",
                kernel_flattens, re_entry_refusals, day_three_refusals);
    // Day 3's block finds a flat book: FlattenAndBlock issues nothing there.
    if (kernel_flattens != 2 || re_entry_refusals == 0 || day_three_refusals == 0) {
        std::cerr << "expected one kernel flatten per intraday breach and both openings refused\n";
        return 1;
    }

    // --- the ledger -------------------------------------------------------------
    for (std::size_t day = 0; day < host.ledger_at_day_end.size(); ++day) {
        const auto& row = host.ledger_at_day_end[day];
        std::printf("day %zu close: blocked=%d reason=%-22s fills_today=%llu loss_days=%u "
                    "day_open_equity=%.2f peak=%.2f\n",
                    day + 1, row.blocked ? 1 : 0, row.reason ? limit_name(*row.reason) : "-",
                    static_cast<unsigned long long>(row.fills_today), row.consecutive_loss_days,
                    row.day_open_equity, row.peak_equity);
    }
    if (host.ledger_at_day_end.size() != 3) return 1;
    const auto& day1 = host.ledger_at_day_end[0];
    const auto& day2 = host.ledger_at_day_end[1];
    const auto& day3 = host.ledger_at_day_end[2];
    if (!day1.blocked || !day1.reason || *day1.reason != no::RiskLimitKind::MaxIntradayLoss
        || day1.fills_today != 2 || day1.consecutive_loss_days != 0
        || !near(day1.day_open_equity, 100000.0)
        || !day2.blocked || !day2.reason || *day2.reason != no::RiskLimitKind::MaxIntradayLoss
        || day2.fills_today != 2 || day2.consecutive_loss_days != 1
        || !near(day2.day_open_equity, 98800.0)
        || !day3.blocked || !day3.reason
        || *day3.reason != no::RiskLimitKind::MaxConsecutiveLossDays
        || day3.fills_today != 0 || day3.consecutive_loss_days != 2
        || !near(day3.day_open_equity, 97800.0) || !near(day3.peak_equity, 100000.0)) {
        std::cerr << "expected the ledger to carry each day's opening equity, fills and streak\n";
        return 1;
    }
    if (host.physical_position().signed_units != 0.0) {
        std::cerr << "expected a flat book at the end\n";
        return 1;
    }

    // Both rows were closed by the account rule, not by the strategy.
    double net = 0.0;
    for (int i = 0; i < host.trade_count(); ++i) net += host.get_trade(i).pnl;
    if (host.trade_count() != 2 || !near(net, -2200.0)
        || std::string(host.get_trade(0).exit_id) != "__kernel_risk__"
        || std::string(host.get_trade(1).exit_id) != "__kernel_risk__") {
        std::cerr << "expected two kernel-closed rows netting -2200\n";
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
    return 0;
}
