// Pine-free native example: the open book lot by lot, and the account
// statistics a host reads beside it (R5 gap lane N18).
//
// strategy.opentrades.* is Pine's view of the still-open book. Its native
// counterpart is one call: native_open_lots(mark) copies out one NativeOpenLot
// per open physical lot, oldest first, with the lot's identity (ordinal, entry
// incarnation, position cycle), its booking facts (label, comment, time, bar,
// price, signed units), the entry fee still on it, and three values computed
// at the price you pass — the live P&L and the two excursions. Pine marks at
// the current close; a native host passes whichever price it means.
//
// The scenario pyramids a long three times and then reduces it partially, on
// flat bars (open = high = low = close) with
// NativeCloseExecution::AfterCalculation, so a request submitted in bar k's
// calculation fills at bar k's own close and every number below is exact:
//
//   idx:      0    1    2    3    4    5    6    7
//   price:  100  100  102  101  104  106  103  101
//     1: Transact +1  "L1"  -> lot @100          5: read the three lots
//     2: Transact +2  "L2"  -> lot @102          6: Reduce 1.5 (FIFO)
//     4: Transact +1  "L3"  -> lot @104          7: read what is left
//
// What the example proves:
//   * the rows are the book: one per lot, oldest first, ordinals 0..n-1, and
//     as many rows as physical_position().lot_count;
//   * they are marked, not stored: unrealized_pnl is the move from entry_price
//     to `mark` less the lot's own entry fee, and the marked equity is exactly
//     the account balance plus the sum of those rows —
//     native_marked_equity(mark) == current_equity() + sum(unrealized_pnl);
//   * a partial close is FIFO and takes its SHARE of the entry fee with it:
//     the 1.5-unit reduce closes all of L1 and a quarter of L2, leaving L2
//     with 1.5 units and 1.5 of its original 2.0 fee;
//   * a NaN mark keeps every booking fact, leaves unrealized_pnl NaN and folds
//     nothing into the excursions — reading the book is an observation and
//     never a decision the run depends on.
//
// The last block is the other half of Pine's report namespace. strategy.equity,
// strategy.netprofit, strategy.grossprofit, strategy.wintrades and the rest are
// PROTECTED accessors on BacktestEngine, which NativeStrategyHost derives from:
// they are reachable from inside your own host, exactly as they are reachable
// from inside a generated Pine strategy, and they are what fill_report() writes
// into pf_report_t for a C caller.
//
//   c++ -std=c++17 native_open_lots_strategy.cpp -lpineforge_kernel -o open_lots
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace no = pineforge::native_order;

constexpr std::int64_t kQuarter = 15LL * 60LL * 1000LL;
constexpr double kFee = 2.0;      // CashPerExecution: one ticket per execution
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

const double kPrices[] = {100.0, 100.0, 102.0, 101.0, 104.0, 106.0, 103.0, 101.0};
constexpr int kBarCount = 8;

bool near(double a, double b) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-9;
}

int failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    std::printf("FAIL: %s\n", what);
}

class OpenLotsExample : public pineforge::NativeStrategyHost {
public:
    // One snapshot per calculation: the rows, the identity they must satisfy,
    // and the account statistics read at the same point.
    struct Snapshot {
        std::vector<pineforge::NativeOpenLot> rows;
        std::vector<pineforge::NativeOpenLot> unmarked;   // the same rows at a NaN mark
        std::size_t lot_count = 0;
        double mark = 0.0;
        double marked_equity = 0.0;
        double balance = 0.0;
    };
    std::vector<Snapshot> snaps;

    // Pine's report namespace, read through the protected accessors this host
    // inherits, at the end of the run.
    struct Statistics {
        double equity = 0.0;          // strategy.equity  (at the final close)
        double netprofit = 0.0;       // strategy.netprofit
        double grossprofit = 0.0;     // strategy.grossprofit
        double grossloss = 0.0;       // strategy.grossloss
        double openprofit = 0.0;      // strategy.openprofit
        double avg_trade = 0.0;       // strategy.avg_trade
        double capital_held = 0.0;    // strategy.opentrades.capital_held
        int wintrades = 0;            // strategy.wintrades
        int losstrades = 0;           // strategy.losstrades
        double position_size = 0.0;   // strategy.position_size
        double max_drawdown = 0.0;    // strategy.max_drawdown
        double max_runup = 0.0;       // strategy.max_runup
        double contracts_held = 0.0;  // strategy.max_contracts_held_all
    };
    Statistics statistics;

private:
    int bar_ = -1;

    void on_native_run_begin() override {
        bar_ = -1;
        snaps.clear();
    }

    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext&) override {
        ++bar_;

        Snapshot snap;
        snap.mark = bar.close;
        snap.rows = native_open_lots(bar.close);
        snap.unmarked = native_open_lots(kNaN);
        snap.lot_count = physical_position().lot_count;
        snap.marked_equity = native_marked_equity(bar.close);
        snap.balance = current_equity();   // protected: initial capital + realized
        snaps.push_back(std::move(snap));

        switch (bar_) {
            case 1: submit({no::Transact{1.0}, "L1", "first"}); break;
            case 2: submit({no::Transact{2.0}, "L2", "second"}); break;
            case 4: submit({no::Transact{1.0}, "L3", "third"}); break;
            case 6: submit({no::Reduce{no::ExplicitUnits{1.5}}, "partial", ""}); break;
            default: break;
        }

        // The protected report accessors, refreshed at every calculation so the
        // last calculation's values are the run's.
        statistics.equity = current_equity() + open_profit(bar.close);
        statistics.netprofit = net_profit();
        statistics.grossprofit = gross_profit();
        statistics.grossloss = gross_loss();
        statistics.openprofit = open_profit(bar.close);
        statistics.avg_trade = avg_trade();
        statistics.capital_held = open_trades_capital_held();
        statistics.wintrades = count_wintrades();
        statistics.losstrades = count_losstrades();
        statistics.position_size = physical_position().signed_units;
        // The equity extremes and the position-size peaks are folded by the
        // kernel's own report point, so they stand at zero unless the run
        // asked for one (NativeReportPolicy::KernelRecorded). Both are
        // protected members of BacktestEngine, like the accessors above.
        statistics.max_drawdown = max_drawdown_;
        statistics.max_runup = max_runup_;
        statistics.contracts_held = max_contracts_held_all();
    }
};

pineforge::NativeRunSpec make_spec(pineforge::NativeReportPolicy policy) {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-open-lots-example";
    spec.identity.run_number = 1;
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
    spec.price_tick = 0.01;
    spec.fee_kind = pineforge::NativeFeeKind::CashPerExecution;
    spec.fee_value = kFee;
    // A market request submitted in bar k's calculation fills at bar k's close.
    spec.close_execution = pineforge::NativeCloseExecution::AfterCalculation;
    spec.report_policy = policy;
    return spec;
}

// Flat bars: every matching point of bar k is that bar's own price.
std::vector<pineforge::Bar> flat_bars() {
    std::vector<pineforge::Bar> bars;
    bars.reserve(kBarCount);
    for (int i = 0; i < kBarCount; ++i) {
        pineforge::Bar bar{};
        bar.open = bar.high = bar.low = bar.close = kPrices[i];
        bar.volume = 4.0;
        bar.timestamp = static_cast<std::int64_t>(i) * kQuarter;
        bars.push_back(bar);
    }
    return bars;
}

void print_rows(const std::vector<pineforge::NativeOpenLot>& rows, double mark) {
    for (const auto& row : rows) {
        std::printf("  lot %zu  %-3s %-2s  units=%+.2f entry=%.2f fee=%.2f  "
                    "pnl@%.2f=%+.2f  runup=%.2f drawdown=%.2f  bar=%d cycle=%lld\n",
                    row.ordinal, row.entry_label.c_str(),
                    row.side == no::Side::Long ? "L" : "S",
                    row.signed_units, row.entry_price, row.entry_commission,
                    mark, row.unrealized_pnl, row.favorable_excursion,
                    row.adverse_excursion, row.entry_bar_index,
                    static_cast<long long>(row.cycle));
    }
}

bool run_once(OpenLotsExample& host, pineforge::NativeReportPolicy policy) {
    if (host.configure_native(make_spec(policy)).status
        != pineforge::NativeSetupStatus::Applied) {
        std::cerr << "configure: " << host.last_error() << '\n';
        return false;
    }
    const std::vector<pineforge::Bar> bars = flat_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));
    if (host.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "run: " << host.last_error() << '\n';
        return false;
    }
    return host.snaps.size() == static_cast<std::size_t>(kBarCount);
}

}  // namespace

int main() {
    // The run this example reads: the kernel records one report point per
    // script calculation, so the equity curve, the equity extremes and the
    // position-size peaks are the kernel's. Recording books no cash and
    // places no order — the run below proves the trades are the same.
    OpenLotsExample host;
    if (!run_once(host, pineforge::NativeReportPolicy::KernelRecorded)) {
        std::cerr << "the KernelRecorded run did not complete with one snapshot per bar\n";
        return 1;
    }

    // --- the invariants that hold at every calculation ----------------------
    for (std::size_t k = 0; k < host.snaps.size(); ++k) {
        const auto& snap = host.snaps[k];
        check(snap.rows.size() == snap.lot_count,
              "one row per lot of physical_position().lot_count");
        double sum = 0.0;
        for (std::size_t i = 0; i < snap.rows.size(); ++i) {
            check(snap.rows[i].ordinal == i, "rows are ordinals 0..n-1, oldest first");
            check(snap.rows[i].favorable_excursion >= 0.0
                  && snap.rows[i].adverse_excursion >= 0.0,
                  "both excursions are magnitudes");
            sum += snap.rows[i].unrealized_pnl;
        }
        check(near(snap.marked_equity, snap.balance + sum),
              "native_marked_equity(mark) == balance + sum(unrealized_pnl)");

        // A NaN mark keeps the booking facts and computes nothing.
        check(snap.unmarked.size() == snap.rows.size(), "a NaN mark still lists the book");
        for (std::size_t i = 0; i < snap.unmarked.size(); ++i) {
            check(snap.unmarked[i].entry_label == snap.rows[i].entry_label
                  && near(snap.unmarked[i].entry_price, snap.rows[i].entry_price)
                  && near(snap.unmarked[i].signed_units, snap.rows[i].signed_units)
                  && near(snap.unmarked[i].entry_commission, snap.rows[i].entry_commission),
                  "a NaN mark keeps every booking fact");
            check(std::isnan(snap.unmarked[i].unrealized_pnl),
                  "a NaN mark leaves unrealized_pnl NaN");
        }
        (void)k;
    }

    // --- bar 5: the pyramided book, three lots, marked at 106 ---------------
    const auto& full = host.snaps[5];
    std::printf("open lots at bar 5 (mark %.2f), marked equity %.2f = balance %.2f + rows\n",
                full.mark, full.marked_equity, full.balance);
    print_rows(full.rows, full.mark);
    check(full.rows.size() == 3, "three lots after three openings");
    if (full.rows.size() == 3) {
        const auto& a = full.rows[0];
        const auto& b = full.rows[1];
        const auto& c = full.rows[2];
        check(a.entry_label == "L1" && a.entry_comment == "first"
              && near(a.entry_price, 100.0) && near(a.signed_units, 1.0)
              && near(a.entry_commission, kFee) && a.entry_bar_index == 1,
              "lot 0 is L1: one unit at 100, its own 2.00 ticket, opened on bar 1");
        check(b.entry_label == "L2" && near(b.entry_price, 102.0)
              && near(b.signed_units, 2.0) && near(b.entry_commission, kFee),
              "lot 1 is L2: two units at 102 on one ticket");
        check(c.entry_label == "L3" && near(c.entry_price, 104.0)
              && near(c.signed_units, 1.0) && near(c.entry_commission, kFee),
              "lot 2 is L3: one unit at 104");
        // (106 - entry) * units - the fee still on the lot.
        check(near(a.unrealized_pnl, 4.0), "L1 marks +4.00 at 106 (6.00 gross less its 2.00 fee)");
        check(near(b.unrealized_pnl, 6.0), "L2 marks +6.00 at 106");
        check(near(c.unrealized_pnl, 0.0), "L3 marks 0.00 at 106");
        check(a.side == no::Side::Long && a.entry_incarnation != 0
              && a.cycle == b.cycle && b.cycle == c.cycle,
              "one long position cycle, and every lot names the request that opened it");
    }

    // --- bar 7: after the FIFO partial, the fee share went with the slice ----
    const auto& rest = host.snaps[7];
    std::printf("open lots at bar 7 (mark %.2f), after Reduce 1.5 filled at 103.00\n", rest.mark);
    print_rows(rest.rows, rest.mark);
    check(rest.rows.size() == 2, "FIFO closed L1 whole and split L2");
    if (rest.rows.size() == 2) {
        check(rest.rows[0].entry_label == "L2" && near(rest.rows[0].signed_units, 1.5)
              && near(rest.rows[0].entry_price, 102.0),
              "the remainder of L2 is 1.5 units, still at its own entry price");
        check(near(rest.rows[0].entry_commission, kFee * 0.75),
              "the closed quarter took a quarter of L2's entry fee with it");
        check(rest.rows[1].entry_label == "L3" && near(rest.rows[1].signed_units, 1.0)
              && near(rest.rows[1].entry_commission, kFee),
              "L3 is untouched");
    }

    // --- the report namespace, read through the protected accessors ---------
    const auto& stats = host.statistics;
    std::printf("report at the last calculation: equity=%.4f netprofit=%.4f "
                "grossprofit=%.4f grossloss=%.4f openprofit=%.4f avg_trade=%.4f\n",
                stats.equity, stats.netprofit, stats.grossprofit, stats.grossloss,
                stats.openprofit, stats.avg_trade);
    std::printf("                             wintrades=%d losstrades=%d eventrades=%d "
                "max_contracts_held=%.2f capital_held=%.2f position_size=%+.2f\n",
                stats.wintrades, stats.losstrades, host.eventrades(),
                host.max_contracts_held_all(), stats.capital_held, stats.position_size);
    check(near(stats.netprofit, stats.grossprofit + stats.grossloss),
          "netprofit is grossprofit plus grossloss (the loss term is signed)");
    check(stats.wintrades + stats.losstrades + host.eventrades() == host.trade_count(),
          "every closed row is a win, a loss or an even trade");
    check(near(stats.contracts_held, 4.0),
          "the book peaked at four units (1 + 2 + 1)");
    check(near(stats.position_size, 2.5), "2.5 units are still open at the end");

    // --- who folds the extremes: the report policy, and nothing else --------
    //
    // max_drawdown / max_runup / max_contracts_held_* are folded at the
    // kernel's own report point, which exists only under
    // NativeReportPolicy::KernelRecorded (or KernelRecordedAtHostMarks). The
    // default HostRecorded leaves the whole report series to the host, so a
    // bare host that never asks sees them at zero — and sees exactly the same
    // trades, because recording is reporting.
    OpenLotsExample bare;
    if (!run_once(bare, pineforge::NativeReportPolicy::HostRecorded)) {
        std::cerr << "the HostRecorded run did not complete with one snapshot per bar\n";
        return 1;
    }
    const auto& bare_stats = bare.statistics;
    std::printf("report policy: KernelRecorded folds max_drawdown=%.4f max_runup=%.4f "
                "max_contracts_held=%.2f; HostRecorded folds %.4f / %.4f / %.2f\n",
                stats.max_drawdown, stats.max_runup, stats.contracts_held,
                bare_stats.max_drawdown, bare_stats.max_runup, bare_stats.contracts_held);
    // The marked equity peaks at bar 5 and only falls after it, so this tape's
    // run-up is legitimately zero and the drawdown is the whole fall.
    check(near(stats.max_drawdown, 16.5) && near(stats.max_runup, 0.0)
          && near(stats.contracts_held, 4.0),
          "KernelRecorded folds the equity extremes and the position-size peak");
    check(bare_stats.max_drawdown == 0.0 && bare_stats.max_runup == 0.0
          && bare_stats.contracts_held == 0.0,
          "HostRecorded folds none of them: that series is the host's");
    check(bare.trade_count() == host.trade_count()
          && near(bare_stats.netprofit, stats.netprofit),
          "recording moves no fill: the two runs close the same trades for the same money");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
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
                  << " commission=" << trade.commission << '\n';
    }
    return host.trade_count() > 0 ? 0 : 1;
}
