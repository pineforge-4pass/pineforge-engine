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

// The tapes' script: 10 000 of capital, percent-of-equity 100 by default,
// margin 100 both ways, no fee, a one-share grid (NYSE:F's lot).
class Host : public pineforge::source::PineStrategyHost {
public:
    explicit Host(bool cancel) : cancel_(cancel) {
        initial_capital_ = 10000.0;
        syminfo_mintick_ = 0.01;
        qty_step_ = 1.0;
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
            strategy_exit("X", "S", kNaN, 10.25);
        }
        if (at(c, 4, 30, 15, 45)) {
            strategy_entry("L", true);
            strategy_close("S", "Reverse to Long");
        }
        if (cancel_ && at(c, 5, 1, 9, 30)) strategy_cancel("X");
    }
    using BacktestEngine::range_end_trades_;
private:
    bool cancel_;
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
    Host host(false);
    host.run(feed.data(), static_cast<int>(feed.size()));
    CHECK(host.last_error().empty());
    // badapter-v19dp1-control.
    check_rows(host, {
        {12.0, 10.39, 5, 2, 9, 30, "__margin_call__", "Margin call", -3.48},
        {928.0, 10.39, 5, 2, 9, 30, "X", "", -269.12},
    });
    CHECK(host.live_position_size() == 0.0);
}

void a_cancelled_exit_is_not_revived() {
    const auto feed = bars();
    Host host(true);
    host.run(feed.data(), static_cast<int>(feed.size()));
    CHECK(host.last_error().empty());
    // badapter-v19dp1-cancel: two margin-call slices, X never closes.
    check_rows(host, {
        {12.0, 10.39, 5, 2, 9, 30, "__margin_call__", "Margin call", -3.48},
        {92.0, 10.57, 5, 6, 10, 0, "__margin_call__", "Margin call", -43.24},
    });
    std::printf("    open at the end: %.4f\n", host.live_position_size());
    CHECK(host.live_position_size() == -836.0);
    CHECK(host.range_end_trades_.size() == 1);
    CHECK(!host.range_end_trades_.empty() && host.range_end_trades_[0].qty == 836.0);
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
    std::printf("R5 B-ADAPTER margin revival after a cancel: %d checks, %d failures\n", checks,
                failures);
    return failures ? 1 : 0;
}
