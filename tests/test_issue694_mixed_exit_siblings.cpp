/*
 * One strategy.exit placed in the calculation of its market strategy.entry
 * can give one level as a price and the other as a tick distance:
 *
 *     strategy.entry("E", strategy.long)
 *     strategy.exit("X", "E", profit = 1000, stop = 95)
 *
 * The tick distance can only be priced when the entry fills, so that leg
 * waits for the fill; the priced level is an order of its own from the call.
 * At the fill the waiting leg is priced from the fill price, and the priced
 * level the caller gave must still stand beside it. In Pine v6, when
 * both prices for one leg are specified, the one reached first applies.
 *
 * Every row drives the real source host over real bars and reads the trade
 * the engine closed. The expected exit of a row is computed here from that
 * row's own inputs: the caller's priced level, or the fill bar's open plus
 * or minus the tick distance. Rows marked "red-on-base" fail on a tree that
 * loses or overrides the priced level at the fill; rows marked "control"
 * hold on both trees.
 *
 * Output: four lines per row (RUN, OBSERVED, EXPECTED, PASS or FAIL) and one
 * SUMMARY line. Exit status 1 when a row failed.
 */

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;
constexpr double kQty = 2.0;
constexpr std::int64_t kFirstBarMs = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kBarMs = 60000LL;

int rows_run = 0;
int rows_failed = 0;

Bar bar_at(int index, double open, double high, double low, double close) {
    Bar bar{};
    bar.open = open;
    bar.high = high;
    bar.low = low;
    bar.close = close;
    bar.volume = 1000.0;
    bar.timestamp = kFirstBarMs + static_cast<std::int64_t>(index) * kBarMs;
    return bar;
}

// The entry is called on bar 0 and fills at the open of bar 1 (100). Then
// the lows reach 98 (bar 2), 95 (bar 3) and 90 (bar 4) before the highs
// reach 105 and 110 (bar 5).
std::vector<Bar> falling_then_rising() {
    return {
        bar_at(0, 100.0, 100.5, 99.5, 100.0),
        bar_at(1, 100.0, 100.5, 99.5, 100.0),
        bar_at(2, 100.0, 100.5, 97.0, 99.0),
        bar_at(3, 99.0, 99.5, 94.0, 96.0),
        bar_at(4, 96.0, 96.5, 89.0, 90.0),
        bar_at(5, 90.0, 111.0, 89.5, 110.0),
        bar_at(6, 110.0, 110.5, 109.5, 110.0),
    };
}

// The mirror: the highs reach 102 (bar 2), 105 (bar 3) and 110 (bar 4)
// before the lows reach 95 and 90 (bar 5).
std::vector<Bar> rising_then_falling() {
    return {
        bar_at(0, 100.0, 100.5, 99.5, 100.0),
        bar_at(1, 100.0, 100.5, 99.5, 100.0),
        bar_at(2, 100.0, 103.0, 99.5, 101.0),
        bar_at(3, 101.0, 106.0, 100.5, 104.0),
        bar_at(4, 104.0, 111.0, 103.5, 110.0),
        bar_at(5, 110.0, 110.5, 89.0, 90.0),
        bar_at(6, 90.0, 90.5, 89.5, 90.0),
    };
}

// One strategy.exit call: a level is a price, a tick distance, or absent.
struct ExitCall {
    double limit = kNaN;
    double stop = kNaN;
    double profit_ticks = kNaN;
    double loss_ticks = kNaN;
};

ExitCall exit_call(double limit, double stop, double profit_ticks, double loss_ticks) {
    ExitCall call;
    call.limit = limit;
    call.stop = stop;
    call.profit_ticks = profit_ticks;
    call.loss_ticks = loss_ticks;
    return call;
}

struct Script {
    bool is_long = true;
    // The exit's from_entry: the entry id, or "" for an exit that names none.
    std::string from_entry = "E";
    // The exit calls made right after strategy.entry, in the calculation of
    // bar 0, in order.
    std::vector<ExitCall> with_entry;
    // The exit calls made at the close of bar 1, the position open.
    std::vector<ExitCall> after_fill;
};

class MixedExitHost final : public source::PineStrategyHost {
public:
    explicit MixedExitHost(Script script) : script_(std::move(script)) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config{};
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = kQty;
        config.pyramiding = 1;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_mintick(kTick);
    }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar == 0) {
            strategy_entry("E", script_.is_long, kNaN, kNaN, kQty);
            for (const ExitCall& call : script_.with_entry) place(call);
        } else if (bar == 1) {
            open_size_bar1_ = open_trade_size(0);
            position_bar1_ = signed_position_size();
            for (const ExitCall& call : script_.after_fill) place(call);
        }
    }

    double open_size_bar1() const { return open_size_bar1_; }
    double position_bar1() const { return position_bar1_; }
    double closed_size(int index) const { return closed_trade_size(index); }

private:
    void place(const ExitCall& call) {
        strategy_exit("X", script_.from_entry, call.limit, call.stop, kNaN, kNaN, kNaN, 100.0,
                      "", kNaN, "", call.profit_ticks, call.loss_ticks);
    }

    Script script_;
    double open_size_bar1_ = kNaN;
    double position_bar1_ = kNaN;
};

struct Outcome {
    std::string error;
    std::string entry_id;
    std::string exit_id;
    int closed = 0;     // closed trades, the range-end mark of an open one left out
    int open_rows = 0;  // range-end marks: a position the run never closed
    bool is_long = false;
    double entry_price = kNaN;
    double exit_price = kNaN;
    double quantity = kNaN;
    int entry_bar = -1;
    int exit_bar = -1;
    double closed_size = kNaN;     // strategy.closedtrades.size(0)
    double open_size_bar1 = kNaN;  // strategy.opentrades.size(0), close of bar 1
    double position_bar1 = kNaN;   // strategy.position_size, close of bar 1
};

Outcome run_script(const Script& script, const std::vector<Bar>& bars) {
    MixedExitHost host(script);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    Outcome out;
    out.error = host.last_error();
    out.open_size_bar1 = host.open_size_bar1();
    out.position_bar1 = host.position_bar1();
    for (int index = 0; index < host.report_trade_count(); ++index) {
        const Trade trade = host.get_report_trade(index);
        if (trade.open_at_end) {
            ++out.open_rows;
            continue;
        }
        if (out.closed == 0) {
            out.entry_id = trade.entry_id;
            out.exit_id = trade.exit_id;
            out.is_long = trade.is_long;
            out.entry_price = trade.entry_price;
            out.exit_price = trade.exit_price;
            out.quantity = trade.qty;
            out.entry_bar = trade.entry_bar_index;
            out.exit_bar = trade.exit_bar_index;
            out.closed_size = host.closed_size(index);
        }
        ++out.closed;
    }
    return out;
}

bool near(double left, double right) {
    return std::isfinite(left) && std::isfinite(right) && std::fabs(left - right) <= 1e-6;
}

const char* side_name(bool is_long) { return is_long ? "long" : "short"; }

// The entry is a market order called on bar 0: it fills at bar 1's open.
double fill_price(const std::vector<Bar>& bars) { return bars[1].open; }

// One closed trade of the whole entry quantity, at `want_exit`.
void check_exit(const char* row, const char* kind, const Script& script,
                const std::vector<Bar>& bars, double want_exit, int want_exit_bar = -1) {
    const Outcome got = run_script(script, bars);
    const double want_entry = fill_price(bars);
    const bool pass = got.error.empty() && got.closed == 1 && got.open_rows == 0
        && got.entry_id == "E" && got.exit_id == "X" && got.is_long == script.is_long
        && got.entry_bar == 1 && near(got.entry_price, want_entry)
        && near(got.exit_price, want_exit) && near(got.quantity, kQty)
        && (want_exit_bar < 0 || got.exit_bar == want_exit_bar);
    ++rows_run;
    if (!pass) ++rows_failed;
    std::printf("[694-B1] ROW %s RUN kind=%s\n", row, kind);
    std::printf("[694-B1] ROW %s OBSERVED closed=%d open_rows=%d side=%s entry=%.8f exit=%.8f "
                "qty=%.8f entry_bar=%d exit_bar=%d ids=\"%s\",\"%s\" error=\"%s\"\n",
                row, got.closed, got.open_rows, side_name(got.is_long), got.entry_price,
                got.exit_price, got.quantity, got.entry_bar, got.exit_bar, got.entry_id.c_str(),
                got.exit_id.c_str(), got.error.c_str());
    std::printf("[694-B1] ROW %s EXPECTED closed=1 open_rows=0 entry_bar=1 side=%s "
                "entry=%.8f exit=%.8f qty=%.8f ids=\"E\",\"X\"",
                row, side_name(script.is_long), want_entry, want_exit, kQty);
    if (want_exit_bar >= 0) std::printf(" exit_bar=%d", want_exit_bar);
    std::printf("\n");
    std::printf("[694-B1] ROW %s %s\n", row, pass ? "PASS" : "FAIL");
}

// Behaviour 4: strategy.opentrades.size and strategy.closedtrades.size carry
// the position's sign. Read through the host's accessors on a run whose
// position is open at bar 1 and closed by the end.
void check_signed_sizes(const char* open_row, const char* closed_row, const Script& script,
                        const std::vector<Bar>& bars) {
    const Outcome got = run_script(script, bars);
    const double want = script.is_long ? kQty : -kQty;
    const bool open_pass = got.error.empty() && near(got.open_size_bar1, want)
        && near(got.position_bar1, want);
    const bool closed_pass = got.error.empty() && got.closed == 1 && near(got.closed_size, want);
    rows_run += 2;
    if (!open_pass) ++rows_failed;
    if (!closed_pass) ++rows_failed;
    std::printf("[694-B4] ROW %s RUN kind=control\n", open_row);
    std::printf("[694-B4] ROW %s OBSERVED opentrades_size=%.8f position_size=%.8f error=\"%s\"\n",
                open_row, got.open_size_bar1, got.position_bar1, got.error.c_str());
    std::printf("[694-B4] ROW %s EXPECTED opentrades_size=%.8f position_size=%.8f\n", open_row,
                want, want);
    std::printf("[694-B4] ROW %s %s\n", open_row, open_pass ? "PASS" : "FAIL");
    std::printf("[694-B4] ROW %s RUN kind=control\n", closed_row);
    std::printf("[694-B4] ROW %s OBSERVED closed=%d closedtrades_size=%.8f error=\"%s\"\n",
                closed_row, got.closed, got.closed_size, got.error.c_str());
    std::printf("[694-B4] ROW %s EXPECTED closed=1 closedtrades_size=%.8f\n", closed_row, want);
    std::printf("[694-B4] ROW %s %s\n", closed_row, closed_pass ? "PASS" : "FAIL");
}

Script script_of(bool is_long, const std::string& from_entry, std::vector<ExitCall> with_entry,
                 std::vector<ExitCall> after_fill = {}) {
    Script script;
    script.is_long = is_long;
    script.from_entry = from_entry;
    script.with_entry = std::move(with_entry);
    script.after_fill = std::move(after_fill);
    return script;
}

class CrossoverExitHost final : public source::PineStrategyHost {
public:
    explicit CrossoverExitHost(bool priced_target) : priced_target_(priced_target) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config{};
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 10.0;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_type("crypto");
        set_syminfo_mintick(kTick);
        set_syminfo_metadata("qty_step", 0.00001);
        set_syminfo_metadata("chart_ema_na_warmup", 1.0);
    }

    void on_source_bar(const Bar& bar) override {
        if (cross_.compute(fast_.compute(bar.close), slow_.compute(bar.close))) {
            strategy_entry("Long", true);
            strategy_exit("Long exit", "Long", priced_target_ ? bar.close * 1.012 : kNaN,
                          bar.close * 0.99, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                          priced_target_ ? kNaN : 50000.0, kNaN);
        }
    }

private:
    bool priced_target_;
    ta::EMA fast_{9};
    ta::EMA slow_{21};
    ta::Crossover cross_;
};

// The checked-in BTCUSDT hourly feed, 2024-01-01 through 2024-03-01.
std::vector<Bar> btc_hours() {
    const std::string source = __FILE__;
    std::ifstream input(source.substr(0, source.find_last_of("/\\") + 1)
                        + "fixtures/qty_step_mincontract/btc60/feed/bars.csv");
    std::string line;
    if (!std::getline(input, line) || line != "timestamp,open,high,low,close,volume") return {};
    std::vector<Bar> bars;
    while (std::getline(input, line)) {
        if (std::count(line.begin(), line.end(), ',') != 5) return {};
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream row(line);
        Bar bar{};
        if (!(row >> bar.timestamp >> bar.open >> bar.high >> bar.low >> bar.close >> bar.volume)
            || !(row >> std::ws).eof()
            || bar.timestamp != kFirstBarMs + static_cast<std::int64_t>(bars.size()) * 3600000) {
            return {};
        }
        bars.push_back(bar);
    }
    if (input.bad() || bars.size() != 1441) return {};
    return bars;
}

void check_crossover_exit(const std::vector<Bar>& bars, bool priced_target) {
    const char* row = priced_target ? "crossover.priced-bracket" : "crossover.tick-target";
    ++rows_run;
    std::printf("ROW %s RUN kind=red-on-base\n", row);
    if (bars.size() != 1441) {
        ++rows_failed;
        std::printf("ROW %s NOT COMPARED: missing or malformed BTCUSDT hourly feed\n", row);
        std::printf("ROW %s FAIL feed precondition\n", row);
        return;
    }
    CrossoverExitHost host(priced_target);
    host.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    int passed_stops = 0;
    int witness_count = 0;
    Trade witness{};
    bool valid = host.last_error().empty();
    for (int i = 0; i < host.report_trade_count(); ++i) {
        const Trade trade = host.get_report_trade(i);
        const int entry = trade.entry_bar_index;
        const int exit = trade.open_at_end ? static_cast<int>(bars.size()) : trade.exit_bar_index;
        if (!trade.is_long || trade.entry_id != "Long" || !(trade.qty > 0.0)
            || entry < 1 || entry >= static_cast<int>(bars.size())
            || exit < entry || exit > static_cast<int>(bars.size())
            || (!trade.open_at_end && (exit == static_cast<int>(bars.size())
                                       || trade.exit_id != "Long exit"))) {
            valid = false;
            continue;
        }
        const double stop = bars[entry - 1].close * 0.99;
        if (std::any_of(bars.begin() + entry, bars.begin() + exit,
                        [stop](const Bar& bar) { return bar.low <= stop; })) {
            ++passed_stops;
        }
        if (entry == 118 && !trade.open_at_end) {
            witness = trade;
            ++witness_count;
        }
    }
    const bool pass = valid && passed_stops == 0 && witness_count == 1
        && near(witness.entry_price, 43946.26) && witness.exit_bar_index == 126
        && near(witness.exit_price, 43506.79)
        && (!priced_target || host.report_trade_count() == 29);
    std::printf("ROW %s OBSERVED trades=%d passed_stops=%d bar118_rows=%d entry=%.2f "
                "exit_bar=%d exit=%.2f error=\"%s\"\n", row, host.report_trade_count(),
                passed_stops, witness_count, witness.entry_price, witness.exit_bar_index,
                witness.exit_price, host.last_error().c_str());
    std::printf("ROW %s EXPECTED passed_stops=0 bar118_rows=1 entry=43946.26 "
                "exit_bar=126 exit=43506.79%s\n", row, priced_target ? " trades=29" : "");
    std::printf("ROW %s %s\n", row, pass ? "PASS" : "FAIL");
    if (!pass) ++rows_failed;
}

// Both legs belong to the same reservation. On the path 94 -> 111 -> 85,
// the tick target must not take the unit left for the later stop.
void check_reserved_pair() {
    class Host final : public source::PineStrategyHost {
    public:
        Host() {
            attach_pine_execution_adapter();
            set_syminfo_mintick(kTick);
        }
        void on_source_bar(const Bar&) override {
            if (pine_bar_index() != 0) return;
            strategy_entry("E", true, kNaN, kNaN, 2.0);
            strategy_exit("Pair", "E", kNaN, 95.0, kNaN, kNaN, kNaN,
                          100.0, "", 1.0, "", 1000.0);
            strategy_exit("Remainder", "E", kNaN, 90.0, kNaN, kNaN, kNaN, 100.0, "", 2.0);
        }
    };
    const auto bars = std::vector<Bar>{
        bar_at(0, 100, 101, 99, 100),
        bar_at(1, 100, 101, 94, 97),
        bar_at(2, 97, 111, 96, 110),
        bar_at(3, 110, 110, 85, 90),
    };
    const char* row = "reservation.mixed-pair";
    Host host;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    const auto closed = host.closed_trade_count();
    const bool two = closed == 2 && host.report_trade_count() == 2;
    const Trade first = closed >= 1 ? host.get_trade(0) : Trade{};
    const Trade last = closed >= 2 ? host.get_trade(1) : Trade{};
    const bool pass = two && host.last_error().empty()
        && first.exit_id == "Pair" && last.exit_id == "Remainder"
        && first.entry_bar_index == 1 && last.entry_bar_index == 1
        && first.exit_bar_index == 1 && last.exit_bar_index == 3
        && near(first.exit_price, 95.0) && near(last.exit_price, 90.0)
        && near(first.qty, 1.0) && near(last.qty, 1.0);
    ++rows_run;
    std::printf("ROW %s RUN kind=red-on-base\n", row);
    std::printf("ROW %s OBSERVED closed=%zu reports=%d %s@%.2f x%.2f bar=%d; "
                "%s@%.2f x%.2f bar=%d error=\"%s\"\n", row, host.closed_trade_count(),
                host.report_trade_count(), first.exit_id.c_str(), first.exit_price, first.qty,
                first.exit_bar_index, last.exit_id.c_str(), last.exit_price, last.qty,
                last.exit_bar_index, host.last_error().c_str());
    std::printf("ROW %s EXPECTED Pair@95 x1 bar=1; Remainder@90 x1 bar=3\n", row);
    std::printf("ROW %s %s\n", row, pass ? "PASS" : "FAIL");
    if (!pass) ++rows_failed;
}

}  // namespace

int main() {
    const auto hours = btc_hours();
    check_crossover_exit(hours, false);
    check_crossover_exit(hours, true);
    check_reserved_pair();
    const std::vector<Bar> falling = falling_then_rising();
    const std::vector<Bar> rising = rising_then_falling();
    const double long_fill = fill_price(falling);
    const double short_fill = fill_price(rising);

    // ---- A tick distance beside a priced level of the other kind ----------
    // Long, profit in ticks, stop as a price: the stop is the caller's 95.
    check_exit("B1.long.profit-ticks+priced-stop", "red-on-base",
               script_of(true, "E", {exit_call(kNaN, 95.0, 1000.0, kNaN)}), falling, 95.0);
    // Long, limit as a price, loss in ticks: the limit is the caller's 110.
    check_exit("B1.long.priced-limit+loss-ticks", "red-on-base",
               script_of(true, "E", {exit_call(110.0, kNaN, kNaN, 500.0)}), rising, 110.0);
    // Short, profit in ticks, stop as a price: the stop is the caller's 105.
    check_exit("B1.short.profit-ticks+priced-stop", "red-on-base",
               script_of(false, "E", {exit_call(kNaN, 105.0, 1000.0, kNaN)}), rising, 105.0);
    // Short, limit as a price, loss in ticks: the limit is the caller's 90.
    check_exit("B1.short.priced-limit+loss-ticks", "red-on-base",
               script_of(false, "E", {exit_call(90.0, kNaN, kNaN, 500.0)}), falling, 90.0);

    // ---- A priced level beside a tick distance of its own kind ------------
    // expectation corrected: 95 -> 98, 105 -> 102 and 110 -> 102, because Pine v6
    // chooses the level reached first when both operands specify the same leg.
    check_exit("long.nearer-tick-stop", "control",
               script_of(true, "E", {exit_call(kNaN, 95.0, 1000.0, 200.0)}), falling, 98.0);
    check_exit("short.nearer-tick-stop", "control",
               script_of(false, "E", {exit_call(kNaN, 105.0, 1000.0, 200.0)}), rising, 102.0);
    check_exit("long.nearer-tick-limit", "control",
               script_of(true, "E", {exit_call(110.0, kNaN, 200.0, 500.0)}), rising, 102.0);
    check_exit("long.nearer-priced-stop", "red-on-base",
               script_of(true, "E", {exit_call(kNaN, 95.0, 1000.0, 800.0)}), falling, 95.0);
    check_exit("short.nearer-priced-limit", "red-on-base",
               script_of(false, "E", {exit_call(95.0, kNaN, 1000.0, 500.0)}), falling, 95.0);

    // ---- The call repeated or changed before the entry fills --------------
    // The second call moves the priced stop to 96: that is the stop at the fill.
    check_exit("B1.long.priced-stop-changed-before-fill", "red-on-base",
               script_of(true, "E", {exit_call(kNaN, 95.0, 1000.0, kNaN),
                                     exit_call(kNaN, 96.0, 1000.0, kNaN)}),
               falling, 96.0);
    check_exit("B1.long.call-repeated-before-fill", "red-on-base",
               script_of(true, "E", {exit_call(kNaN, 95.0, 1000.0, kNaN),
                                     exit_call(kNaN, 95.0, 1000.0, kNaN)}),
               falling, 95.0);
    check_exit("long.replaced-by-prices-before-fill", "red-on-base",
               script_of(true, "E", {exit_call(kNaN, 95.0, 1000.0, kNaN),
                                      exit_call(106.0, 96.0, kNaN, kNaN)}), rising, 106.0);
    auto gapped = rising;
    gapped[0] = bar_at(0, 90.0, 91.0, 89.0, 90.0);
    check_exit("long.tick-target-from-gapped-fill", "control",
               script_of(true, "E", {exit_call(kNaN, 85.0, 1000.0, kNaN)}), gapped,
               fill_price(gapped) + 1000.0 * kTick);

    // ---- The same shape on an exit that names no entry --------------------
    check_exit("B1.unnamed.long.profit-ticks+priced-stop", "red-on-base",
               script_of(true, "", {exit_call(kNaN, 95.0, 1000.0, kNaN)}), falling, 95.0);
    auto swept = rising;
    swept[1] = bar_at(1, 100.0, 111.0, 96.0, 104.0);
    check_exit("short.unnamed.stop-on-entry-bar", "red-on-base",
               script_of(false, "", {exit_call(kNaN, 105.0, 1000.0, kNaN)}), swept, 105.0, 1);
    swept[1] = bar_at(1, 100.0, 111.0, 94.0, 104.0);
    check_exit("long.loss-from-fill-on-entry-bar", "control",
               script_of(true, "E", {exit_call(110.0, kNaN, kNaN, 500.0)}), swept, 95.0, 1);

    // ---- Controls: shapes that already hold --------------------------------
    // Called again at the close of the fill bar, the position open: the call
    // is priced from the position at once.
    check_exit("B1.control.long.call-repeated-after-fill", "control",
               script_of(true, "E", {exit_call(kNaN, 95.0, 1000.0, kNaN)},
                         {exit_call(kNaN, 95.0, 1000.0, kNaN)}),
               falling, 95.0);
    // No stop was ever given: the lows of bars 3 and 4 close nothing, and the
    // profit distance fills from the fill price.
    check_exit("B1.control.long.profit-ticks-only", "control",
               script_of(true, "E", {exit_call(kNaN, kNaN, 1000.0, kNaN)}), falling,
               long_fill + 1000.0 * kTick);
    // The second call leaves the stop out: it replaces the first, so no stop
    // stands at the fill.
    check_exit("B1.control.long.priced-stop-withdrawn-before-fill", "control",
               script_of(true, "E", {exit_call(kNaN, 95.0, 1000.0, kNaN),
                                     exit_call(kNaN, kNaN, 1000.0, kNaN)}),
               falling, long_fill + 1000.0 * kTick);
    check_exit("B1.control.long.priced-levels-only", "control",
               script_of(true, "E", {exit_call(110.0, 95.0, kNaN, kNaN)}), falling, 95.0);
    check_exit("B1.control.short.priced-levels-only", "control",
               script_of(false, "E", {exit_call(90.0, 105.0, kNaN, kNaN)}), rising, 105.0);
    check_exit("B1.control.long.tick-distances-only", "control",
               script_of(true, "E", {exit_call(kNaN, kNaN, 1000.0, 500.0)}), falling,
               long_fill - 500.0 * kTick);
    check_exit("B1.control.short.tick-distances-only", "control",
               script_of(false, "E", {exit_call(kNaN, kNaN, 1000.0, 500.0)}), rising,
               short_fill + 500.0 * kTick);

    // ---- Behaviour 2: a market entry and, in the same calculation, an exit
    // in ticks that names no entry. It takes effect at the entry's fill.
    check_exit("B2.control.unnamed.tick-distances-only", "control",
               script_of(true, "", {exit_call(kNaN, kNaN, 1000.0, 500.0)}), falling,
               long_fill - 500.0 * kTick);
    check_exit("B2.control.unnamed.priced-levels-only", "control",
               script_of(true, "", {exit_call(110.0, 95.0, kNaN, kNaN)}), falling, 95.0);

    // ---- Behaviour 4: signed open and closed trade sizes -------------------
    check_signed_sizes("B4.control.long.opentrades-size", "B4.control.long.closedtrades-size",
                       script_of(true, "E", {exit_call(110.0, 95.0, kNaN, kNaN)}), falling);
    check_signed_sizes("B4.control.short.opentrades-size", "B4.control.short.closedtrades-size",
                       script_of(false, "E", {exit_call(90.0, 105.0, kNaN, kNaN)}), rising);

    std::printf("[694-B1] SUMMARY rows=%d passed=%d failed=%d\n", rows_run,
                rows_run - rows_failed, rows_failed);
    return rows_failed == 0 ? 0 : 1;
}
