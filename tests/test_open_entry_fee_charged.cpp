/*
 * test_open_entry_fee_charged.cpp — lane W8E-EXITS.
 *
 * TradingView charges an entry's commission when the entry fills. While lots
 * are open, the entry fees they have paid (the share still on each lot after
 * a partial close) are already out of strategy.netprofit, and so out of
 * strategy.equity and of the netprofit / openprofit percentages;
 * strategy.openprofit stays gross of them. The Pine host used to read the
 * closed trades' net profit only, so every script that sized or decided from
 * strategy.equity while a position was open, with a commission declared, ran
 * on an equity one open entry fee too high (the definededge reversal
 * quantities, 0.054 % of a 0.06 % fee on 90 % of equity).
 *
 * Each row replays TradingView's own tape of a synthetic probe
 * (tests/fixtures/open_entry_fee_charged, lab tv exports on BINANCE:ETHUSDT.P
 * 15) through the Pine host under the configuration the generated constructor
 * declares for it, over the corpus 15m bars embedded in bars.inc. A probe
 * opens a position, reads the accessors while it is open, and later encodes
 * each value as the quantity of a one-bar trade; every trade the tape closes
 * must be the engine's: entry and exit time, side, price and quantity. The
 * accessors are spelled as the generated code spells them
 * (pineforge_codegen emit_top.py / visit_expr.py): strategy.equity is
 * current_equity() + open_profit(close), strategy.netprofit net_profit(),
 * strategy.netprofit_percent net_profit() / initial_capital_ * 100 and
 * strategy.openprofit_percent open_profit(close) / current_equity() * 100.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
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

#ifndef PINEFORGE_OPEN_ENTRY_FEE_FIXTURE_DIR
#error "PINEFORGE_OPEN_ENTRY_FEE_FIXTURE_DIR must name tests/fixtures/open_entry_fee_charged"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/open_entry_fee_charged/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:ETHUSDT.P quantity step and price tick.
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;
// The bar before every tape's first entry (2025-04-01 12:15 UTC), where its
// order was placed; the harness trades from here, as run_strategy.py does
// for a corpus tape.
constexpr std::int64_t kTradeStartMs = 1743508800000LL;  // 2025-04-01 12:00 UTC

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry ms, long, entry price, quantity, exit ms, exit price).
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kEth15) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    return bars;
}

// 2025-04-01 <hour>:<minute> UTC, the probes' timestamp("UTC", 2025, 4, 1, ...) cells.
std::int64_t at(int hour, int minute) {
    return kEth15[0].ts + (static_cast<std::int64_t>(hour) * 60 + minute) * 60'000;
}

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

struct Tape {
    std::vector<Row> trades;             // in trade-number order
    std::vector<std::string> entries;    // each trade's entry signal
    std::vector<double> commissions;     // each trade's commission column
};

Tape read_tape(const std::string& tape) {
    std::ifstream in(std::string(PINEFORGE_OPEN_ENTRY_FEE_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
    std::map<int, Row> by_number;
    std::map<int, std::string> entry_signal;
    std::map<int, double> commission;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        // Trade number, Type, Date and time, Signal, Price USDT, Size (qty),
        // Size (value), Net PnL USDT, Return %, Commission USDT, ...
        if (cell.size() < 10) continue;
        const int number = std::stoi(cell[0]);
        Row& row = by_number[number];
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(std::stod(cell[4]));
            std::get<3>(row) = lots(std::stod(cell[5]));
            entry_signal[number] = cell[3];
            commission[number] = std::stod(cell[9]);
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(std::stod(cell[4]));
        }
    }
    Tape out;
    for (const auto& [number, row] : by_number) {
        out.trades.push_back(row);
        out.entries.push_back(entry_signal[number]);
        out.commissions.push_back(commission[number]);
    }
    return out;
}

enum class Probe { SingleLot, PyramidPartial, CashFee };

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, const source::PineStrategyConfig& config)
        : probe_(probe) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        switch (probe_) {
        case Probe::SingleLot:
            if (t == at(12, 0)) strategy_entry("A", true, kNaN, kNaN, 10);
            if (t == at(13, 0)) {
                read_accessors();
                strategy_close("A");
            }
            break;
        case Probe::PyramidPartial:
            if (t == at(12, 0)) strategy_entry("A", true, kNaN, kNaN, 10);
            if (t == at(12, 30)) strategy_entry("B", true, kNaN, kNaN, 6);
            if (t == at(13, 0)) strategy_close("A", {}, kNaN, 50.0);
            if (t == at(13, 45)) read_accessors();
            if (t == at(14, 0)) strategy_close("");
            break;
        case Probe::CashFee:
            if (t == at(12, 0)) strategy_entry("A", true, kNaN, kNaN, 10);
            if (t == at(13, 0)) {
                read_accessors();
                strategy_close("A");
            }
            break;
        }
        // Each value read while the position was open, as a one-bar trade:
        // long when it is at least zero, of |value| / 10 (or * 10, * 1000
        // for the percentages) plus 0.00003 lots against a binary64 floor.
        encode("OP", op_, 15, [](double v) { return std::abs(v) / 10 + 3e-05; });
        encode("EQ", eq_ - initial_capital_, 16, [](double v) { return std::abs(v) / 10 + 3e-05; });
        encode("NP", np_, 17, [](double v) { return std::abs(v) / 10 + 3e-05; });
        if (probe_ == Probe::SingleLot) {
            encode("NPP", npp_, 18, [](double v) { return std::abs(v) * 10 + 3e-05; });
            encode("OPP", opp_, 19, [](double v) { return std::abs(v) * 1000 + 3e-05; });
        }
    }

private:
    void read_accessors() {
        op_ = open_profit(current_bar_.close);                   // strategy.openprofit
        eq_ = current_equity() + open_profit(current_bar_.close); // strategy.equity
        np_ = net_profit();                                        // strategy.netprofit
        npp_ = (net_profit() / initial_capital_) * 100.0;        // strategy.netprofit_percent
        opp_ = current_equity() != 0.0                            // strategy.openprofit_percent
            ? (open_profit(current_bar_.close) / current_equity()) * 100.0 : 0.0;
    }

    template <class Size>
    void encode(const char* id, double value, int hour, Size size) {
        const std::int64_t t = current_bar_.timestamp;
        if (t == at(hour, 0)) strategy_entry(id, value >= 0.0, kNaN, kNaN, size(value));
        if (t == at(hour, 15)) strategy_close(id);
    }

    Probe probe_;
    double op_ = kNaN, eq_ = kNaN, np_ = kNaN, npp_ = kNaN, opp_ = kNaN;
};

std::vector<Row> run(Probe probe, const source::PineStrategyConfig& config, std::string* error) {
    ProbeHost host(probe, config);
    host.set_trade_start_time(kTradeStartMs);
    const std::vector<Bar> bars = feed();
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    *error = host.last_error();
    std::vector<Row> out;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        out.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty),
                         t.exit_time, ticks(t.exit_price));
    }
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %lld %s @%lld ticks q=%lld lots -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

source::PineStrategyConfig config(CommissionType type, double value, int pyramiding) {
    source::PineStrategyConfig c{};
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.commission_type = static_cast<int>(type);
    c.commission_value = value;
    c.pyramiding = pyramiding;
    return c;
}

struct Case {
    const char* tape;
    Probe probe;
    const char* declares;
    source::PineStrategyConfig lane;
    std::size_t trades;
};

// A value the tape encodes: |value| / 10 + 0.00003 lots, floored to TradingView's lot.
double decoded(const Tape& tape, std::size_t row, double scale) {
    const double qty = static_cast<double>(std::get<3>(tape.trades[row])) * kLot;
    return (std::get<1>(tape.trades[row]) ? 1.0 : -1.0) * qty * scale;
}

}  // namespace

int main() {
    constexpr CommissionType kPercent = CommissionType::PERCENT;
    const Case cases[] = {
        {"s5-equity-fee-pct", Probe::SingleLot, "commission percent 1", config(kPercent, 1.0, 0), 6},
        {"s5c-equity-nofee-pct", Probe::SingleLot, "commission percent 0", config(kPercent, 0.0, 0), 4},
        {"s2-netprofit-pyramid-partial", Probe::PyramidPartial,
         "commission percent 1, pyramiding 2", config(kPercent, 1.0, 2), 6},
        {"s3-netprofit-cash-order", Probe::CashFee, "commission cash_per_order 25",
         config(CommissionType::CASH_PER_ORDER, 25.0, 0), 4},
        {"s4-netprofit-cash-contract", Probe::CashFee, "commission cash_per_contract 3",
         config(CommissionType::CASH_PER_CONTRACT, 3.0, 0), 4},
    };

    std::map<std::string, Tape> tapes;
    for (const Case& c : cases) {
        std::printf("-- %s (%s)\n", c.tape, c.declares);
        const Tape tape = read_tape(c.tape);
        tapes[c.tape] = tape;
        CHECK(tape.trades.size() == c.trades);
        std::string error;
        const std::vector<Row> engine = run(c.probe, c.lane, &error);
        CHECK(error.empty());
        CHECK(engine == tape.trades);
        if (engine != tape.trades) {
            show("tape", tape.trades);
            show("engine", engine);
        }
    }

    // The rule, read off TradingView's own rows. A's entry fee is its entry
    // price times ten units times one percent; the encoded values are floored
    // to the lot, so each comparison allows one lot of the encoding.
    std::printf("-- the rule, on the tapes\n");
    {
        const Tape& fee = tapes["s5-equity-fee-pct"];
        const Tape& free = tapes["s5c-equity-nofee-pct"];
        if (fee.trades.size() == 6 && free.trades.size() == 4) {
            const double entry_fee = static_cast<double>(std::get<2>(fee.trades[0])) * kTick * 10 * 0.01;
            const double op = decoded(fee, 1, 10);
            const double eq = decoded(fee, 2, 10);
            const double np = decoded(fee, 3, 10);
            // strategy.openprofit is gross: the same value with or without the fee.
            CHECK(std::get<3>(fee.trades[1]) == std::get<3>(free.trades[1]));
            CHECK(std::get<1>(fee.trades[1]) == std::get<1>(free.trades[1]));
            // strategy.netprofit is minus the open entry fee, and equity carries it.
            CHECK(std::abs(np + entry_fee) < 10 * kLot * 10);
            CHECK(std::abs(eq - (op + np)) < 2 * 10 * kLot * 10);
            // netprofit_percent over the initial capital; openprofit_percent
            // over the capital plus that charged net profit.
            CHECK(std::abs(decoded(fee, 4, 0.1) - np / 1000.0) < 2 * kLot * 0.1);
            CHECK(std::abs(decoded(fee, 5, 0.001) - op / (100000.0 + np) * 100.0) < 2 * kLot * 0.001);
            // Without a fee strategy.netprofit is zero: no NP trade opens.
            for (const std::string& signal : free.entries) CHECK(signal != "NP" && signal != "NPP");
        }
        const Tape& pyramid = tapes["s2-netprofit-pyramid-partial"];
        if (pyramid.trades.size() == 6) {
            // The partial close's row took half of A's entry fee; the rest of
            // A's and all of B's is charged to the open-position net profit.
            const double a_fee = static_cast<double>(std::get<2>(pyramid.trades[0])) * kTick * 10 * 0.01;
            const double b_fee = static_cast<double>(std::get<2>(pyramid.trades[2])) * kTick * 6 * 0.01;
            const double closed_row = -187.9225;  // tape row 1's Net PnL
            CHECK(std::abs(decoded(pyramid, 5, 10) - (closed_row - a_fee / 2 - b_fee)) < 2 * kLot * 10);
        }
        CHECK(tapes["s3-netprofit-cash-order"].trades.size() == 4
              && std::abs(decoded(tapes["s3-netprofit-cash-order"], 3, 10) + 25.0) < 2 * kLot * 10);
        CHECK(tapes["s4-netprofit-cash-contract"].trades.size() == 4
              && std::abs(decoded(tapes["s4-netprofit-cash-contract"], 3, 10) + 30.0) < 2 * kLot * 10);
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
