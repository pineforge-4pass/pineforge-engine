/*
 * test_adapter_margin_revival_cancel.cpp -- R5 lane B-ADAPTER, item 7
 * (V19D-P1): a margin call does not revive an exit the script cancelled.
 *
 * TradingView (finding-311 and round 7 family M, restated by the adapter as
 * revive_brackets_after_margin) holds a short's standing strategy.exit
 * dormant once a same-bar reversal pair is declined, and a margin-call slice
 * revives it: when the revived stop is already reached at the margin call's
 * price, the whole remaining position closes there through the exit's id.
 * The legacy engine revived only live pending orders -- a cancelled order was
 * skipped (ab9714be pine_fills.cpp:2065-2076, `o.cancellation.cancelled()`) --
 * while the adapter revived a dormant exit whatever the script had done to
 * it since: R5 lane V19-D's directed variant 2 closed a position through an
 * exit strategy.cancel had cancelled.
 *
 * TradingView decides it. Two lab tv tapes on NYSE:F 15 (2025-04-15 ..
 * 2025-05-20, range proof covered), one script with and without its cancel:
 *   - 04-29 15:45 ET: strategy.entry("S", short, qty=940) and
 *     strategy.exit("X", "S", stop=10.25); the short fills 04-30 09:30 @10.10;
 *   - 04-30 15:45: strategy.entry("L", long) and strategy.close("S") -- the
 *     05-01 09:30 open gaps to 10.15 and the pair is declined;
 *   - (cancel only) 05-01 09:30: strategy.cancel("X");
 *   - 05-02 09:30 the high 10.385 passes the short's margin-call level.
 * badapter-v19dp1-control: "Margin call" 12 @10.39, then X 928 @10.39 on the
 * same bar. badapter-v19dp1-cancel: "Margin call" 12 @10.39 on 05-02, a
 * second "Margin call" 92 @10.57 on 05-06 10:00, and 836 still open when the
 * chart ends -- the cancelled X never comes back. Both runs below replay the
 * same script on the same bars (the fixture) and must book exactly the tape.
 *
 * R5 lane V19-FIX (audit X2): the adapter still revived an exit the script
 * cancelled while it was LIVE. strategy.cancel retired the lifecycle of a
 * dormant exit only, so once the declined pair's suspension made the
 * cancelled leg dormant on 05-01, the margin call read it as a standing exit
 * and closed 928 through it. Two more lab tv tapes on the same chart and
 * window decide it, each the cancel tape's script with the cancel moved:
 *   - v19fix-cancel-before-pair: strategy.cancel("X") on 04-30 15:45, before
 *     the pair (tv_trades sha256 662ce8c8..., pine 1d6c305b..., range proof
 *     covered);
 *   - v19fix-cancel-at-1000: strategy.cancel("X") on 04-30 10:00, the bar
 *     after the short fills (tv_trades sha256 662ce8c8..., pine 13ed512b...,
 *     range proof covered).
 * Both book exactly the cancel tape (the same tv_trades.csv bytes): margin
 * calls 12 @10.39 and 92 @10.57, 836 open. The other spellings of the audit's
 * variants follow the same rule and book the same trades: strategy.cancel_all
 * on the cancel tape's bar and before the pair, and strategy.cancel after the
 * pair on its bar. The cancel now retires the lifecycle of every exit it takes
 * off the book as well as every dormant one it names.
 *
 * The adapter has a second road back. When the 05-01 open gaps through X's
 * stop, the declined reversal takes X off the book at the pair's command and
 * parks it for the next margin-call slice (apply_reversal_gap_bracket_policy,
 * pending_margin_revivals_). Three more tapes with X's stop at 10.15, which
 * the 05-01 09:30 open (10.15) gaps through:
 *   - v19fix-gapped-stop-control: no cancel -- "Margin call" 12 @10.39 and X
 *     928 @10.39 on 05-02 (tv_trades sha256 bdf3d470...);
 *   - v19fix-gapped-stop-cancel: strategy.cancel("X") on 05-01 09:30;
 *   - v19fix-gapped-stop-cancel-after-pair: strategy.cancel("X") on 04-30
 *     15:45 after the pair, in the callback that parked X;
 * both cancels book the cancel tape's file again (662ce8c8...). A cancel now
 * drops the parked revival and withdraws every exit row its id names.
 *
 * A chart with no quantity grid moves the pair's hold to the command itself
 * (strategy.close's all-in reversal pair holds the standing bracket at once),
 * so there the hold and the cancel share one callback: before the lane a
 * cancel after the pair was refused by the lifecycle as a conflicting replay
 * of the hold's step, and one before it was overwritten by the hold. No tape
 * covers such a chart; the rows below hold the tapes' rule on it -- every
 * cancel spelling books what the cancel on the dormant exit books there, and
 * the same chart without a cancel still closes through X.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "oracle_fixture_config_shim.hpp"
#include "test_adapter_margin_revival_cancel_data.hpp"

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                      \
    } while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// New York wall clock of an epoch millisecond: EDT (UTC-4) throughout the
// fixture's window.
struct Clock {
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
};

Clock new_york(std::int64_t ms) {
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000 - 4 * 3600);
    std::tm t{};
    gmtime_r(&seconds, &t);
    return Clock{t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min};
}

bool at(const Clock& c, int month, int day, int hour, int minute) {
    return c.month == month && c.day == day && c.hour == hour && c.minute == minute;
}

// Where the script cancels X, and how.
enum class Cancel {
    None,          // tape badapter-v19dp1-control
    Dormant,       // tape badapter-v19dp1-cancel: strategy.cancel("X"), 05-01 09:30
    DormantAll,    // strategy.cancel_all(), 05-01 09:30
    BeforePair,    // tape v19fix-cancel-before-pair: strategy.cancel("X"), 04-30 15:45
    AfterPair,     // strategy.cancel("X"), 04-30 15:45, after the pair
    AllBeforePair, // strategy.cancel_all(), 04-30 15:45, before the pair
    Earlier,       // tape v19fix-cancel-at-1000: strategy.cancel("X"), 04-30 10:00
};

const char* spelling(Cancel cancel) {
    switch (cancel) {
    case Cancel::None: return "no cancel";
    case Cancel::Dormant: return "strategy.cancel on 05-01 09:30 (X dormant)";
    case Cancel::DormantAll: return "strategy.cancel_all on 05-01 09:30 (X dormant)";
    case Cancel::BeforePair: return "strategy.cancel on 04-30 15:45 before the pair (X live)";
    case Cancel::AfterPair: return "strategy.cancel on 04-30 15:45 after the pair";
    case Cancel::AllBeforePair: return "strategy.cancel_all on 04-30 15:45 before the pair";
    case Cancel::Earlier: return "strategy.cancel on 04-30 10:00 (X live)";
    }
    return "?";
}

// The tapes' script: 10 000 of capital, percent-of-equity 100 by default,
// margin 100 both ways, no fee, a one-share grid (NYSE:F's lot) unless
// `grid` is false.
class Host : public pineforge::source::PineStrategyHost {
public:
    explicit Host(Cancel cancel, bool grid = true, double stop = 10.25)
        : cancel_(cancel), stop_(stop) {
        initial_capital_ = 10000.0;
        syminfo_mintick_ = 0.01;
        qty_step_ = grid ? 1.0 : 0.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        pyramiding_ = 1;
        set_syminfo_timezone("America/New_York");
        set_syminfo_session("0930-1600");
        set_margin_call_enabled(true);
    }
    void on_source_bar(const Bar& bar) override {
        const Clock c = new_york(bar.timestamp);
        if (at(c, 4, 29, 15, 45)) {
            strategy_entry("S", false, kNaN, kNaN, 940.0);
            strategy_exit("X", "S", kNaN, stop_);
        }
        if (cancel_ == Cancel::Earlier && at(c, 4, 30, 10, 0)) strategy_cancel("X");
        if (at(c, 4, 30, 15, 45)) {
            if (cancel_ == Cancel::BeforePair) strategy_cancel("X");
            if (cancel_ == Cancel::AllBeforePair) strategy_cancel_all();
            strategy_entry("L", true);
            strategy_close("S", "Reverse to Long");
            if (cancel_ == Cancel::AfterPair) strategy_cancel("X");
        }
        if (cancel_ == Cancel::Dormant && at(c, 5, 1, 9, 30)) strategy_cancel("X");
        if (cancel_ == Cancel::DormantAll && at(c, 5, 1, 9, 30)) strategy_cancel_all();
    }
    using BacktestEngine::range_end_trades_;
private:
    Cancel cancel_;
    double stop_;
};

std::vector<Bar> bars() {
    std::vector<Bar> out;
    for (const auto& row : margin_revival_cancel_data::kBars) {
        Bar bar;
        bar.timestamp = static_cast<std::int64_t>(row[0]);
        bar.open = row[1];
        bar.high = row[2];
        bar.low = row[3];
        bar.close = row[4];
        bar.volume = row[5];
        out.push_back(bar);
    }
    return out;
}

struct Row {
    double qty;
    double exit_price;
    int month, day, hour, minute;
    const char* exit_id;
    const char* comment;
    double pnl;
};

void check_rows(const Host& host, const std::vector<Row>& tape) {
    CHECK(host.trade_count() == static_cast<int>(tape.size()));
    for (int k = 0; k < host.trade_count(); ++k) {
        const Trade& trade = host.get_trade(k);
        const Clock exit = new_york(trade.exit_time);
        std::printf("    %s qty %.4f entry @%.3f exit %02d-%02d %02d:%02d @%.3f id '%s' '%s'"
                    " pnl %.2f\n", trade.is_long ? "long" : "short", trade.qty,
                    trade.entry_price, exit.month, exit.day, exit.hour, exit.minute,
                    trade.exit_price, trade.exit_id.c_str(), trade.exit_comment.c_str(),
                    trade.pnl);
        if (k >= static_cast<int>(tape.size())) continue;
        const Row& want = tape[static_cast<std::size_t>(k)];
        CHECK(!trade.is_long);
        CHECK(trade.qty == want.qty);
        CHECK(std::abs(trade.entry_price - 10.10) < 1e-9);
        CHECK(std::abs(trade.exit_price - want.exit_price) < 1e-9);
        CHECK(at(exit, want.month, want.day, want.hour, want.minute));
        CHECK(trade.exit_id == want.exit_id);
        CHECK(trade.exit_comment == want.comment);
        CHECK(std::abs(trade.pnl - want.pnl) < 1e-9);
    }
}

void the_revival_closes_through_a_live_dormant_exit() {
    const auto feed = bars();
    Host host(Cancel::None);
    host.run(feed.data(), static_cast<int>(feed.size()));
    CHECK(host.last_error().empty());
    // badapter-v19dp1-control.
    check_rows(host, {
        {12.0, 10.39, 5, 2, 9, 30, "__margin_call__", "Margin call", -3.48},
        {928.0, 10.39, 5, 2, 9, 30, "X", "", -269.12},
    });
    CHECK(host.live_position_size() == 0.0);
}

// badapter-v19dp1-cancel, v19fix-cancel-before-pair and v19fix-cancel-at-1000
// (one tv_trades.csv): two margin-call slices, X never closes, 836 open.
void books_the_cancel_tape(Cancel cancel, double stop = 10.25) {
    const auto feed = bars();
    Host host(cancel, true, stop);
    host.run(feed.data(), static_cast<int>(feed.size()));
    CHECK(host.last_error().empty());
    std::printf("   %s, stop %.2f\n", spelling(cancel), stop);
    check_rows(host, {
        {12.0, 10.39, 5, 2, 9, 30, "__margin_call__", "Margin call", -3.48},
        {92.0, 10.57, 5, 6, 10, 0, "__margin_call__", "Margin call", -43.24},
    });
    std::printf("    open at the end: %.4f\n", host.live_position_size());
    CHECK(host.live_position_size() == -836.0);
    CHECK(host.range_end_trades_.size() == 1);
    CHECK(!host.range_end_trades_.empty() && host.range_end_trades_[0].qty == 836.0);
}

void a_cancelled_exit_is_not_revived() { books_the_cancel_tape(Cancel::Dormant); }

void an_exit_cancelled_live_before_the_pair_is_not_revived() {
    books_the_cancel_tape(Cancel::BeforePair);
}

void an_exit_cancelled_live_on_an_earlier_bar_is_not_revived() {
    books_the_cancel_tape(Cancel::Earlier);
}

void every_cancel_spelling_books_the_cancel_tape() {
    for (Cancel cancel : {Cancel::DormantAll, Cancel::AfterPair, Cancel::AllBeforePair})
        books_the_cancel_tape(cancel);
}

// The gapped stop: without a cancel the parked X closes the rest at the
// margin call (v19fix-gapped-stop-control) ...
void a_gapped_stop_parked_for_the_margin_call_closes_through_x() {
    const auto feed = bars();
    Host host(Cancel::None, true, 10.15);
    host.run(feed.data(), static_cast<int>(feed.size()));
    CHECK(host.last_error().empty());
    check_rows(host, {
        {12.0, 10.39, 5, 2, 9, 30, "__margin_call__", "Margin call", -3.48},
        {928.0, 10.39, 5, 2, 9, 30, "X", "", -269.12},
    });
    CHECK(host.live_position_size() == 0.0);
}

// ... and a cancel keeps it parked for good (v19fix-gapped-stop-cancel,
// v19fix-gapped-stop-cancel-after-pair; cancel_all and the cancel before the
// pair untaped).
void a_cancelled_gapped_stop_is_not_revived() {
    for (Cancel cancel : {Cancel::Dormant, Cancel::AfterPair, Cancel::DormantAll,
                          Cancel::AllBeforePair, Cancel::BeforePair})
        books_the_cancel_tape(cancel, 10.15);
}

struct Booked {
    std::vector<Trade> trades;
    double open = 0.0;
    int through_x = 0;
    std::string error;
};

Booked run_without_grid(Cancel cancel) {
    const auto feed = bars();
    Host host(cancel, false);
    host.run(feed.data(), static_cast<int>(feed.size()));
    Booked out;
    for (int k = 0; k < host.trade_count(); ++k) {
        out.trades.push_back(host.get_trade(k));
        if (out.trades.back().exit_id == "X") ++out.through_x;
    }
    out.open = host.live_position_size();
    out.error = host.last_error();
    return out;
}

bool same_trades(const Booked& a, const Booked& b) {
    if (a.trades.size() != b.trades.size() || a.open != b.open) return false;
    for (std::size_t k = 0; k < a.trades.size(); ++k) {
        const Trade& x = a.trades[k];
        const Trade& y = b.trades[k];
        if (x.is_long != y.is_long || x.qty != y.qty || x.entry_price != y.entry_price
            || x.exit_price != y.exit_price || x.entry_time != y.entry_time
            || x.exit_time != y.exit_time || x.exit_id != y.exit_id || x.pnl != y.pnl) {
            return false;
        }
    }
    return true;
}

// No quantity grid: the pair holds X at the command, in the cancel's callback.
void without_a_grid_every_cancel_books_the_dormant_cancel() {
    const Booked standing = run_without_grid(Cancel::None);
    const Booked dormant = run_without_grid(Cancel::Dormant);
    CHECK(standing.error.empty());
    CHECK(dormant.error.empty());
    std::printf("   no cancel: %zu trades, %d through X, open %.4f\n", standing.trades.size(),
                standing.through_x, standing.open);
    std::printf("   dormant cancel: %zu trades, %d through X, open %.4f\n",
                dormant.trades.size(), dormant.through_x, dormant.open);
    // Not vacuous: without a cancel the margin call closes through X ...
    CHECK(standing.through_x == 1);
    CHECK(standing.open == 0.0);
    // ... and the cancel on the dormant exit keeps it closed.
    CHECK(dormant.through_x == 0);
    CHECK(dormant.open < 0.0);
    CHECK(dormant.trades.size() >= 2);
    for (Cancel cancel : {Cancel::BeforePair, Cancel::AfterPair, Cancel::AllBeforePair,
                          Cancel::Earlier, Cancel::DormantAll}) {
        const Booked booked = run_without_grid(cancel);
        std::printf("   %s: %zu trades, %d through X, open %.4f\n", spelling(cancel),
                    booked.trades.size(), booked.through_x, booked.open);
        CHECK(booked.error.empty());
        CHECK(booked.through_x == 0);
        CHECK(same_trades(booked, dormant));
    }
}

void test(const char* name, void (*fn)()) {
    const int before = failures;
    std::printf("-- %s\n", name);
    fn();
    if (failures != before) std::printf("   ^^ %d failure(s)\n", failures - before);
}

}  // namespace

int main() {
    test("a margin call revives a dormant exit the script left standing (tape: control)",
         the_revival_closes_through_a_live_dormant_exit);
    test("a margin call does not revive an exit the script cancelled (tape: cancel)",
         a_cancelled_exit_is_not_revived);
    test("nor one it cancelled while live, before the pair (tape: v19fix-cancel-before-pair)",
         an_exit_cancelled_live_before_the_pair_is_not_revived);
    test("nor one it cancelled while live, on an earlier bar (tape: v19fix-cancel-at-1000)",
         an_exit_cancelled_live_on_an_earlier_bar_is_not_revived);
    test("cancel_all, and a cancel after the pair, book the cancel tape too",
         every_cancel_spelling_books_the_cancel_tape);
    test("a gapped stop parked for the margin call closes through X (tape: gapped-stop-control)",
         a_gapped_stop_parked_for_the_margin_call_closes_through_x);
    test("a cancelled gapped stop is not revived (tapes: gapped-stop-cancel, -cancel-after-pair)",
         a_cancelled_gapped_stop_is_not_revived);
    test("without a quantity grid (the pair holds X at the command) no cancel is revived",
         without_a_grid_every_cancel_books_the_dormant_cancel);
    std::printf("R5 B-ADAPTER / V19-FIX margin revival after a cancel: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
