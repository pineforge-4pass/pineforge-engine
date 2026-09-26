/*
 * test_cash_fee_sizing_tapes.cpp — R5 lane PAR-CASHFEE.
 *
 * TradingView sizes a percent-of-equity DEFAULT quantity under a cash
 * commission from strategy.equity -- every open entry fee charged, the cash
 * ones included -- and leaves out of that money the fee the order itself pays:
 *
 *   cash_per_order     units = floor((pct * E - fee) / (price * pointvalue))
 *   cash_per_contract  units = floor(pct * E / (price * pointvalue + fee))
 *
 * on the symbol's quantity grid, `price` being the price the quantity is sized
 * at: the signal close for a market strategy.entry and for strategy.order, the
 * level for a priced entry. Before this lane the Pine adapter took the
 * percentage of an equity that restored the open entries' cash fees and
 * reserved none for the order (default_sizing_cash), so every sized quantity
 * came out too large.
 *
 * The tapes are `lab tv` exports (channel ws-report-v1, rangeProof covered)
 * under tests/fixtures/cash_fee_sizing, whose README.md lists them; the bars are
 * the scenario windows of their feeds (fixtures/cash_fee_sizing/bars.inc). The
 * host below runs each tape's script through the Pine adapter.
 *
 * 1. Eleven tapes the adapter books trade for trade: percentages 50, 100 and
 *    200, long and short, flat, pyramided (the open lots' fee shares charged),
 *    after a partial close and reversing (one fee for the reversing order),
 *    priced entries and strategy.order, point values 1 (BINANCE:ETHUSDT.P) and
 *    50 (CME_MINI:ES1!), and lane H-THIN's FIFO fee-basis tape. Every trade's
 *    entry id, side, times, prices and quantity equal the tape's, its
 *    commission to 1e-9 and its net profit to 1e-7 of |profit| + |commission|
 *    (the tape's rounding of a million-dollar profit). 111 trades.
 * 2. Two tapes at default 100 % with margin 100 / 100 (pcf-*-p100-m100): the
 *    quantity the adapter sizes is TradingView's (the first scenario books
 *    TradingView's trade; the second scenario's quantity is the sum of
 *    TradingView's pieces), but TradingView then margin-calls a sliver of that
 *    long on its own entry bar, fired by the order's cash fee, and the adapter
 *    does not (its margin equity restores a cash entry fee,
 *    NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission). This is a
 *    RECORDED divergence, not a sizing question: the adapter's rows are pinned
 *    exactly, so the tapes fail loudly the day the margin equity changes and
 *    must be re-pinned deliberately.
 */

#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#ifndef PINEFORGE_CASH_FEE_FIXTURE_DIR
#error "PINEFORGE_CASH_FEE_FIXTURE_DIR must name tests/fixtures/cash_fee_sizing"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close;
};

#include "fixtures/cash_fee_sizing/bars.inc"

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::int64_t utc_ms(int month, int day, int hour, int minute) {
    const std::int64_t days = days_from_civil(2025, static_cast<unsigned>(month),
                                              static_cast<unsigned>(day));
    return ((days * 24 + hour) * 60 + minute) * 60'000;
}

// ── the scripts ──────────────────────────────────────────────────────

// One statement of a tape's script, issued on the bar that OPENS at the
// step's UTC time (the script's `at(...)` guard).
enum class Op {
    Long, Short,              // strategy.entry(id, dir), default quantity
    LongQty1,                 // strategy.entry(id, long, qty=1)
    LimitLongBelow3,          // strategy.entry(id, long, limit=close - 3)
    LimitShortAbove3,         // strategy.entry(id, short, limit=close + 3)
    StopLongAbove3,           // strategy.entry(id, long, stop=close + 3)
    StopShortBelow3,          // strategy.entry(id, short, stop=close - 3)
    OrderLong, OrderShort,    // strategy.order(id, dir), default quantity
    CloseHalf,                // strategy.close(id, qty_percent=50)
    ExitLimitQtyPositionLess1,  // strategy.exit(id, from, qty=position-1, limit=close*0.9)
    ExitLimit,                // strategy.exit(id, from, limit=close*0.9)
    CloseAll,                 // if strategy.position_size != 0: strategy.close_all()
};

struct Step {
    int month, day, hour, minute;
    Op op;
    const char* id;
    const char* from;
};

std::vector<Step> pyramid_steps() {
    return {
        {4, 2, 10, 0, Op::Long, "A1", ""}, {4, 2, 10, 30, Op::Long, "A2", ""},
        {4, 2, 11, 0, Op::Long, "A3", ""}, {4, 2, 12, 0, Op::CloseAll, "", ""},
        {4, 4, 10, 0, Op::Short, "B1", ""}, {4, 4, 10, 30, Op::Short, "B2", ""},
        {4, 4, 11, 0, Op::Short, "B3", ""}, {4, 4, 12, 0, Op::CloseAll, "", ""},
        {4, 8, 10, 0, Op::Long, "C1", ""}, {4, 8, 10, 30, Op::CloseHalf, "C1", ""},
        {4, 8, 11, 0, Op::Long, "C2", ""}, {4, 8, 12, 0, Op::CloseAll, "", ""},
        {4, 10, 10, 0, Op::Long, "D1", ""}, {4, 10, 10, 30, Op::Short, "D2", ""},
        {4, 10, 12, 0, Op::CloseAll, "", ""},
        {4, 15, 10, 0, Op::Short, "E1", ""}, {4, 15, 10, 30, Op::Long, "E2", ""},
        {4, 15, 12, 0, Op::CloseAll, "", ""},
        {4, 17, 10, 0, Op::Long, "F1", ""}, {4, 17, 12, 0, Op::CloseAll, "", ""},
        {4, 22, 10, 0, Op::Short, "G1", ""}, {4, 22, 12, 0, Op::CloseAll, "", ""},
    };
}

std::vector<Step> leverage_steps() {
    return {
        {4, 17, 10, 0, Op::Long, "F1", ""}, {4, 17, 12, 0, Op::CloseAll, "", ""},
        {4, 22, 10, 0, Op::Short, "G1", ""}, {4, 22, 12, 0, Op::CloseAll, "", ""},
        {4, 10, 10, 0, Op::Long, "D1", ""}, {4, 10, 10, 30, Op::Short, "D2", ""},
        {4, 10, 12, 0, Op::CloseAll, "", ""},
        {4, 15, 10, 0, Op::Short, "E1", ""}, {4, 15, 10, 30, Op::Long, "E2", ""},
        {4, 15, 12, 0, Op::CloseAll, "", ""},
    };
}

std::vector<Step> es_steps() {
    return {
        {6, 2, 14, 0, Op::Long, "A1", ""}, {6, 2, 14, 30, Op::Long, "A2", ""},
        {6, 2, 16, 0, Op::CloseAll, "", ""},
        {6, 3, 14, 0, Op::Short, "B1", ""}, {6, 3, 14, 30, Op::Short, "B2", ""},
        {6, 3, 16, 0, Op::CloseAll, "", ""},
        {6, 4, 14, 0, Op::Long, "D1", ""}, {6, 4, 14, 30, Op::Short, "D2", ""},
        {6, 4, 16, 0, Op::CloseAll, "", ""},
        {6, 5, 14, 0, Op::Long, "F1", ""}, {6, 5, 16, 0, Op::CloseAll, "", ""},
    };
}

std::vector<Step> priced_steps() {
    return {
        {4, 23, 10, 0, Op::OrderLong, "O1", ""}, {4, 23, 10, 30, Op::OrderLong, "O2", ""},
        {4, 23, 12, 0, Op::CloseAll, "", ""},
        {4, 24, 10, 0, Op::LimitShortAbove3, "M1", ""}, {4, 24, 12, 0, Op::CloseAll, "", ""},
        {4, 25, 10, 0, Op::StopLongAbove3, "N1", ""}, {4, 25, 12, 0, Op::CloseAll, "", ""},
        {4, 28, 10, 0, Op::StopShortBelow3, "V1", ""}, {4, 28, 12, 0, Op::CloseAll, "", ""},
        {4, 29, 10, 0, Op::LimitLongBelow3, "K1", ""}, {4, 29, 12, 0, Op::CloseAll, "", ""},
        {4, 30, 10, 0, Op::OrderShort, "W1", ""}, {4, 30, 12, 0, Op::CloseAll, "", ""},
    };
}

std::vector<Step> margin_steps() {
    return {
        {4, 2, 10, 0, Op::Long, "P1", ""}, {4, 2, 12, 0, Op::CloseAll, "", ""},
        {4, 3, 10, 0, Op::Long, "Q1", ""}, {4, 3, 12, 0, Op::CloseAll, "", ""},
        {4, 4, 10, 0, Op::Short, "R1", ""}, {4, 4, 12, 0, Op::CloseAll, "", ""},
        {4, 10, 10, 0, Op::Long, "T1", ""}, {4, 10, 10, 30, Op::Short, "T2", ""},
        {4, 10, 12, 0, Op::CloseAll, "", ""},
        {4, 12, 10, 0, Op::Long, "U1", ""}, {4, 12, 12, 0, Op::CloseAll, "", ""},
    };
}

// Lane H-THIN's X15(b) tape: a FIFO strategy.exit(from_entry) closes an older
// lot of another id before the next default entry sizes.
std::vector<Step> fifo_steps() {
    return {
        {4, 8, 10, 0, Op::LongQty1, "L2A", ""}, {4, 8, 10, 30, Op::Long, "LA", ""},
        {4, 8, 11, 0, Op::ExitLimitQtyPositionLess1, "XA", "LA"},
        {4, 8, 11, 30, Op::Long, "NA", ""}, {4, 8, 12, 30, Op::CloseAll, "", ""},
        {4, 10, 10, 0, Op::LongQty1, "L2B", ""}, {4, 10, 10, 30, Op::Long, "LB", ""},
        {4, 10, 11, 0, Op::ExitLimit, "XB", "LB"},
        {4, 10, 11, 30, Op::Long, "NB", ""}, {4, 10, 12, 30, Op::CloseAll, "", ""},
        {4, 14, 10, 0, Op::Long, "LC", ""}, {4, 14, 10, 30, Op::LongQty1, "L2C", ""},
        {4, 14, 11, 0, Op::ExitLimit, "XC", "LC"},
        {4, 14, 11, 30, Op::Long, "NC", ""}, {4, 14, 12, 30, Op::CloseAll, "", ""},
    };
}

enum class Market { Eth, Es };

struct Tape {
    const char* slug;
    CommissionType commission;
    double commission_value;
    double percent;
    int pyramiding;
    double margin;      // margin_long == margin_short
    Market market;
    double capital;
    std::vector<Step> steps;
};

// ── the host, running a tape's script through the Pine adapter ───────

class TapeHost final : public source::PineStrategyHost {
public:
    explicit TapeHost(const Tape& tape) : tape_(tape) {
        source::PineStrategyConfig config;
        config.initial_capital = tape.capital;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = tape.percent;
        config.pyramiding = tape.pyramiding;
        config.process_orders_on_close = false;
        config.commission_type = static_cast<int>(tape.commission);
        config.commission_value = tape.commission_value;
        config.slippage = 0;
        config.margin_long = tape.margin;
        config.margin_short = tape.margin;
        configure_pine_strategy(config);
        if (tape.market == Market::Es) {
            set_syminfo_mintick(0.25);
            set_syminfo_pointvalue(50.0);
            set_syminfo_metadata("qty_step", 1.0);
        } else {
            set_syminfo_mintick(0.01);
            set_syminfo_metadata("qty_step", 0.0001);
        }
        for (const auto& step : tape.steps)
            step_ms_.push_back(utc_ms(step.month, step.day, step.hour, step.minute));
    }

    void on_source_bar(const Bar& bar) override {
        const double position = live_position_size();
        for (std::size_t i = 0; i < tape_.steps.size(); ++i) {
            if (step_ms_[i] != current_bar_.timestamp) continue;
            const Step& s = tape_.steps[i];
            switch (s.op) {
            case Op::Long: strategy_entry(s.id, true); break;
            case Op::Short: strategy_entry(s.id, false); break;
            case Op::LongQty1: strategy_entry(s.id, true, kNaN, kNaN, 1.0); break;
            case Op::LimitLongBelow3: strategy_entry(s.id, true, bar.close - 3.0); break;
            case Op::LimitShortAbove3: strategy_entry(s.id, false, bar.close + 3.0); break;
            case Op::StopLongAbove3: strategy_entry(s.id, true, kNaN, bar.close + 3.0); break;
            case Op::StopShortBelow3: strategy_entry(s.id, false, kNaN, bar.close - 3.0); break;
            case Op::OrderLong: strategy_order(s.id, true, kNaN); break;
            case Op::OrderShort: strategy_order(s.id, false, kNaN); break;
            case Op::CloseHalf: strategy_close(s.id, "", kNaN, 50.0); break;
            case Op::ExitLimitQtyPositionLess1:
                strategy_exit(s.id, s.from, bar.close * 0.9, kNaN, kNaN, kNaN, kNaN, 100.0, "",
                              position - 1.0);
                break;
            case Op::ExitLimit: strategy_exit(s.id, s.from, bar.close * 0.9, kNaN); break;
            case Op::CloseAll:
                if (position != 0.0) strategy_close_all();
                break;
            }
        }
    }

private:
    const Tape& tape_;
    std::vector<std::int64_t> step_ms_;
};

std::vector<Trade> replay(const Tape& tape) {
    const FeedBar* rows = tape.market == Market::Es ? kEsBars : kEthBars;
    const std::size_t count = tape.market == Market::Es ? sizeof(kEsBars) / sizeof(kEsBars[0])
                                                        : sizeof(kEthBars) / sizeof(kEthBars[0]);
    std::vector<Bar> bars;
    for (std::size_t i = 0; i < count; ++i)
        bars.push_back({rows[i].open, rows[i].high, rows[i].low, rows[i].close, 0.0, rows[i].ts});
    TapeHost host(tape);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    CHECK(host.last_error().empty());
    std::vector<Trade> trades;
    for (int i = 0; i < host.trade_count(); ++i) trades.push_back(host.get_trade(i));
    return trades;
}

// ── the tape ─────────────────────────────────────────────────────────

struct TapeTrade {
    std::string signal;       // the entry's id
    bool is_long = true;
    std::int64_t entry_ms = 0;
    double entry_price = kNaN;
    std::int64_t exit_ms = 0;
    double exit_price = kNaN;
    std::string exit_signal;
    double qty = kNaN;
    double net_profit = kNaN;
    double commission = kNaN;
};

// "YYYY-MM-DD HH:MM" in the tape's UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

// Columns: Trade number, Type, Date and time, Signal, Price, Size (qty),
// Size (value), Net PnL, Return %, Commission, ...
std::vector<TapeTrade> read_tape(const std::string& slug) {
    std::ifstream in(std::string(PINEFORGE_CASH_FEE_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
    std::vector<TapeTrade> trades;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 10) continue;
        const std::size_t n = static_cast<std::size_t>(std::stoi(cell[0]));
        if (trades.size() < n) trades.resize(n);
        TapeTrade& t = trades[n - 1];
        if (cell[1].rfind("Entry", 0) == 0) {
            t.signal = cell[3];
            t.is_long = cell[1] == "Entry long";
            t.entry_ms = tape_ms(cell[2]);
            t.entry_price = std::stod(cell[4]);
            t.qty = std::stod(cell[5]);
            t.net_profit = std::stod(cell[7]);
            t.commission = std::stod(cell[9]);
        } else {
            t.exit_ms = tape_ms(cell[2]);
            t.exit_price = std::stod(cell[4]);
            t.exit_signal = cell[3];
        }
    }
    return trades;
}

bool same(double a, double b, double tolerance = 1e-9) { return std::fabs(a - b) <= tolerance; }

// Everything the tape states about a trade, at the tape's own precision.
bool books_tape_trade(const Trade& trade, const TapeTrade& tape) {
    return trade.entry_id == tape.signal && trade.is_long == tape.is_long
        && trade.entry_time == tape.entry_ms && trade.exit_time == tape.exit_ms
        && same(trade.entry_price, tape.entry_price) && same(trade.exit_price, tape.exit_price)
        && same(trade.qty, tape.qty)
        && same(trade.commission, tape.commission, 1e-9 * std::fmax(1.0, std::fabs(tape.commission)))
        && same(trade.pnl, tape.net_profit,
                1e-7 * (std::fabs(tape.net_profit) + std::fabs(tape.commission)) + 1e-6);
}

void print_mismatch(const char* slug, std::size_t i, const Trade& e, const TapeTrade& t) {
    std::printf("    %s #%zu engine %s %s %lld..%lld %.10g @%.2f->%.2f pnl %.6f comm %.9f\n"
                "    %s #%zu tape   %s %s %lld..%lld %.10g @%.2f->%.2f pnl %.6f comm %.9f\n",
                slug, i + 1, e.entry_id.c_str(), e.is_long ? "L" : "S",
                static_cast<long long>(e.entry_time), static_cast<long long>(e.exit_time), e.qty,
                e.entry_price, e.exit_price, e.pnl, e.commission, slug, i + 1, t.signal.c_str(),
                t.is_long ? "L" : "S", static_cast<long long>(t.entry_ms),
                static_cast<long long>(t.exit_ms), t.qty, t.entry_price, t.exit_price,
                t.net_profit, t.commission);
}

// ── 1. the tapes the adapter books trade for trade ───────────────────

std::vector<Tape> exact_tapes() {
    const auto order = CommissionType::CASH_PER_ORDER;
    const auto contract = CommissionType::CASH_PER_CONTRACT;
    return {
        {"pcf-order-p50-m25", order, 1000.0, 50.0, 3, 25.0, Market::Eth, 100000.0, pyramid_steps()},
        {"pcf-order-p100-m25", order, 1000.0, 100.0, 3, 25.0, Market::Eth, 100000.0, pyramid_steps()},
        {"pcf-contract-p50-m25", contract, 20.0, 50.0, 3, 25.0, Market::Eth, 100000.0,
         pyramid_steps()},
        {"pcf-contract-p100-m25", contract, 20.0, 100.0, 3, 25.0, Market::Eth, 100000.0,
         pyramid_steps()},
        {"pcf-order-p200-m25", order, 1000.0, 200.0, 1, 25.0, Market::Eth, 100000.0,
         leverage_steps()},
        {"pcf-contract-p200-m25", contract, 20.0, 200.0, 1, 25.0, Market::Eth, 100000.0,
         leverage_steps()},
        {"pcf-es-order-p50-m25", order, 1000000.0, 50.0, 2, 25.0, Market::Es, 1e9, es_steps()},
        {"pcf-es-contract-p50-m25", contract, 1000.0, 50.0, 2, 25.0, Market::Es, 1e9, es_steps()},
        {"pcf-order-priced-m25", order, 1000.0, 50.0, 1, 25.0, Market::Eth, 100000.0,
         priced_steps()},
        {"pcf-contract-priced-m25", contract, 20.0, 50.0, 1, 25.0, Market::Eth, 100000.0,
         priced_steps()},
        {"hthin-x15b-fifo-fee-basis", order, 1000.0, 50.0, 5, 100.0, Market::Eth, 100000.0,
         fifo_steps()},
    };
}

void test_tapes_book_tradingview_trades() {
    int booked = 0;
    int total = 0;
    for (const auto& tape : exact_tapes()) {
        const auto engine = replay(tape);
        const auto tv = read_tape(tape.slug);
        CHECK(!tv.empty());
        CHECK(engine.size() == tv.size());
        for (std::size_t i = 0; i < tv.size() && i < engine.size(); ++i) {
            const bool ok = books_tape_trade(engine[i], tv[i]);
            CHECK(ok);
            if (!ok) print_mismatch(tape.slug, i, engine[i], tv[i]);
            booked += ok;
        }
        total += static_cast<int>(tv.size());
    }
    std::printf("  eleven tapes: %d of %d trades booked as TradingView books them\n", booked, total);
    CHECK(total == 111);
    CHECK(booked == total);
}

// ── 2. margin 100: the sizing agrees, the entry-bar margin call does not ──

struct PinnedRow {
    const char* entry_id;
    const char* exit_id;
    bool is_long;
    std::int64_t entry_ms;
    double entry_price;
    std::int64_t exit_ms;
    double exit_price;
    double qty;
};

// The adapter's rows on each margin-100 tape, pinned (a RECORDED divergence).
const PinnedRow kOrderMarginRows[] = {
    {"P1", "__close__", true, 1743588900000LL, 1880.02, 1743596100000LL, 1865.26, 52.6587},
    {"Q1", "__close__", true, 1743675300000LL, 1811.58, 1743682500000LL, 1797.78, 53.1156},
    {"R1", "__close__", false, 1743761700000LL, 1812.69, 1743768900000LL, 1780.11, 51.5748},
    {"T1", "T2", true, 1744280100000LL, 1601.17, 1744281900000LL, 1590.36, 58.1887},
    {"T2", "__margin_call__", false, 1744281900000LL, 1590.36, 1744281900000LL, 1594.81, 0.0372},
    {"T2", "__margin_call__", false, 1744281900000LL, 1590.36, 1744282800000LL, 1594.00, 2.1648},
    {"T2", "__margin_call__", false, 1744281900000LL, 1590.36, 1744286400000LL, 1621.14, 3.542},
    {"T2", "__close__", false, 1744281900000LL, 1590.36, 1744287300000LL, 1595.90, 51.8159},
    {"U1", "__close__", true, 1744452900000LL, 1592.65, 1744460100000LL, 1596.00, 54.084},
};
const PinnedRow kContractMarginRows[] = {
    {"P1", "__close__", true, 1743588900000LL, 1880.02, 1743596100000LL, 1865.26, 52.6307},
    {"Q1", "__close__", true, 1743675300000LL, 1811.58, 1743682500000LL, 1797.78, 53.0244},
    {"R1", "__close__", false, 1743761700000LL, 1812.69, 1743768900000LL, 1780.11, 51.4351},
    {"T1", "T2", true, 1744280100000LL, 1601.17, 1744281900000LL, 1590.36, 57.911},
    {"T2", "__margin_call__", false, 1744281900000LL, 1590.36, 1744281900000LL, 1590.36, 0.0356},
    {"T2", "__margin_call__", false, 1744281900000LL, 1590.36, 1744281900000LL, 1594.81, 1.1732},
    {"T2", "__margin_call__", false, 1744281900000LL, 1590.36, 1744286400000LL, 1621.14, 3.9272},
    {"T2", "__close__", false, 1744281900000LL, 1590.36, 1744287300000LL, 1595.90, 52.0557},
    {"U1", "__close__", true, 1744452900000LL, 1592.65, 1744460100000LL, 1596.00, 54.7167},
};

void check_margin_tape(const Tape& tape, const PinnedRow* pinned, std::size_t pinned_count) {
    const auto engine = replay(tape);
    const auto tv = read_tape(tape.slug);
    CHECK(tv.size() >= 3);
    CHECK(engine.size() == pinned_count);
    if (tv.size() < 3 || engine.empty()) return;
    // P1: the first scenario sizes on the initial capital; TradingView's trade.
    CHECK(books_tape_trade(engine[0], tv[0]));
    // Q1: TradingView fills the quantity the adapter sizes, then margin-calls
    // a sliver of it at the fill bar's open (the order's own cash fee took the
    // equity under the 100 % requirement); the adapter's margin equity restores
    // that fee and books none.
    double tv_q1 = 0.0;
    int tv_q1_margin_calls = 0;
    for (const auto& t : tv) {
        if (t.signal != "Q1") continue;
        tv_q1 += t.qty;
        if (t.exit_signal == "Margin call" && t.exit_ms == t.entry_ms) ++tv_q1_margin_calls;
    }
    CHECK(engine.size() > 1 && engine[1].entry_id == "Q1" && same(engine[1].qty, tv_q1));
    CHECK(tv_q1_margin_calls >= 1);
    CHECK(engine.size() > 1 && engine[1].exit_id == "__close__");
    // The adapter's rows, pinned: the day its margin equity charges the cash
    // entry fee they move, and this tape is re-pinned deliberately.
    for (std::size_t i = 0; i < pinned_count && i < engine.size(); ++i) {
        const auto& e = engine[i];
        const auto& p = pinned[i];
        const bool ok = e.entry_id == p.entry_id && e.exit_id == p.exit_id
            && e.is_long == p.is_long && e.entry_time == p.entry_ms && e.exit_time == p.exit_ms
            && same(e.entry_price, p.entry_price) && same(e.exit_price, p.exit_price)
            && same(e.qty, p.qty);
        CHECK(ok);
        if (!ok) print_mismatch(tape.slug, i, e, tv[std::min(i, tv.size() - 1)]);
    }
}

void test_margin_100_tapes_are_recorded_divergences() {
    const Tape order{"pcf-order-p100-m100", CommissionType::CASH_PER_ORDER, 1000.0, 100.0, 1,
                     100.0, Market::Eth, 100000.0, margin_steps()};
    const Tape contract{"pcf-contract-p100-m100", CommissionType::CASH_PER_CONTRACT, 20.0,
                        100.0, 1, 100.0, Market::Eth, 100000.0, margin_steps()};
    check_margin_tape(order, kOrderMarginRows, sizeof(kOrderMarginRows) / sizeof(kOrderMarginRows[0]));
    check_margin_tape(contract, kContractMarginRows,
                      sizeof(kContractMarginRows) / sizeof(kContractMarginRows[0]));
}

}  // namespace

int main() {
    test_tapes_book_tradingview_trades();
    test_margin_100_tapes_are_recorded_divergences();
    std::printf("PAR-CASHFEE cash-fee sizing tapes: %d checks, %d failures\n",
                tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
