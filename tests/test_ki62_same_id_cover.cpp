/*
 * test_ki62_same_id_cover.cpp — lane W8E-EXITS.
 *
 * KI-62 (ab9714be pine_fills.cpp:7026-7033): when a priced from_entry exit
 * fills on the bar a MARKET pyramid add of its own entry id opened, the exit
 * first closes the id's older lot (FIFO) and then covers the whole same-bar
 * add at its own fill price. The cover ran after any such leg fill, even
 * when the leg had reduced nothing but the add itself -- an id whose only
 * lot is the add, opened beside another id's position. TradingView covers
 * only behind the leg's reduction of an older lot of the id: an entry of
 * another id that fills while a position is open, with its own qty=1 exit
 * filling on that bar, closes one unit and leaves the rest of its lot to its
 * second exit (thulashimohanr's T1/T2 pair on a lot added beside a reversal
 * lot). The engine closed the whole add at the first exit's price. Nor does
 * a leg cover when its FIFO fill reduced only another id's older lot: B's
 * qty=1 exit, filling on B's entry bar, closes one unit of A's lot and leaves
 * both units of B open.
 *
 * Each row replays TradingView's own tape of a synthetic probe
 * (tests/fixtures/ki62_same_id_cover, lab tv exports on BINANCE:ETHUSDT.P 15)
 * through the Pine host under the configuration the generated constructor
 * declares for it, over the corpus 15m bars embedded in bars.inc, and
 * requires every trade the tape closes to be the engine's: entry and exit
 * time, side, price and quantity.
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

#ifndef PINEFORGE_KI62_SAME_ID_FIXTURE_DIR
#error "PINEFORGE_KI62_SAME_ID_FIXTURE_DIR must name tests/fixtures/ki62_same_id_cover"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/ki62_same_id_cover/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;
// The bar before every tape's first entry (2025-04-01 11:45 UTC).
constexpr std::int64_t kTradeStartMs = 1743507000000LL;  // 2025-04-01 11:30 UTC

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

std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

struct Tape {
    std::vector<Row> trades;
    std::vector<std::string> entries;
    std::vector<std::string> exits;
};

Tape read_tape(const std::string& tape) {
    std::ifstream in(std::string(PINEFORGE_KI62_SAME_ID_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
    std::map<int, Row> by_number;
    std::map<int, std::string> entry_signal, exit_signal;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 6) continue;
        const int number = std::stoi(cell[0]);
        Row& row = by_number[number];
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(std::stod(cell[4]));
            std::get<3>(row) = lots(std::stod(cell[5]));
            entry_signal[number] = cell[3];
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(std::stod(cell[4]));
            exit_signal[number] = cell[3];
        }
    }
    Tape out;
    for (const auto& [number, row] : by_number) {
        out.trades.push_back(row);
        out.entries.push_back(entry_signal[number]);
        out.exits.push_back(exit_signal[number]);
    }
    return out;
}

enum class Probe { OtherId, SameId, FifoOtherId };

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, const source::PineStrategyConfig& config) : probe_(probe) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        if (probe_ == Probe::OtherId) {
            if (t == at(11, 30)) {
                strategy_entry("A", true, kNaN, kNaN, 2);
                strategy_exit("AT1", "A", 1861.0, 1800.0, kNaN, kNaN, kNaN, 100.0, {}, 1);
                strategy_exit("AT2", "A", 1869.55, 1800.0, kNaN, kNaN, kNaN, 100.0, {}, 1);
            }
            if (t == at(12, 0)) {
                strategy_entry("B", true, kNaN, kNaN, 2);
                strategy_exit("BT1", "B", 1869.75, 1800.0, kNaN, kNaN, kNaN, 100.0, {}, 1);
                strategy_exit("BT2", "B", 1950.0, 1800.0, kNaN, kNaN, kNaN, 100.0, {}, 1);
            }
        } else if (probe_ == Probe::SameId) {
            if (t == at(11, 30)) {
                strategy_entry("A", true, kNaN, kNaN, 1);
                strategy_exit("AX", "A", 1869.55, 1800.0, kNaN, kNaN, kNaN, 100.0, {}, 1);
            }
            if (t == at(12, 0)) strategy_entry("A", true, kNaN, kNaN, 1);
        } else {
            if (t == at(11, 30)) strategy_entry("A", true, kNaN, kNaN, 1);
            if (t == at(12, 0)) {
                strategy_entry("B", true, kNaN, kNaN, 2);
                strategy_exit("BT1", "B", 1869.75, 1800.0, kNaN, kNaN, kNaN, 100.0, {}, 1);
                strategy_exit("BT2", "B", 1950.0, 1800.0, kNaN, kNaN, kNaN, 100.0, {}, 1);
            }
        }
        if (t == at(13, 30)) strategy_close("");
    }

private:
    Probe probe_;
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

}  // namespace

int main() {
    source::PineStrategyConfig config{};
    config.initial_capital = 1000000.0;
    config.default_qty_type = static_cast<int>(QtyType::FIXED);
    config.default_qty_value = 1.0;
    config.pyramiding = 2;

    struct Case { const char* tape; Probe probe; std::size_t trades; };
    const Case cases[] = {
        {"t1-two-lots-exit-qty1", Probe::OtherId, 4},
        {"t3-same-id-add-cover", Probe::SameId, 2},
        {"t4-fifo-other-id-drain", Probe::FifoOtherId, 2},
    };
    std::map<std::string, Tape> tapes;
    for (const Case& c : cases) {
        std::printf("-- %s\n", c.tape);
        const Tape tape = read_tape(c.tape);
        tapes[c.tape] = tape;
        CHECK(tape.trades.size() == c.trades);
        std::string error;
        const std::vector<Row> engine = run(c.probe, config, &error);
        CHECK(error.empty());
        CHECK(engine == tape.trades);
        if (engine != tape.trades) {
            show("tape", tape.trades);
            show("engine", engine);
        }
    }

    std::printf("-- the rule, on the tapes\n");
    {
        // Another id: B's qty=1 exit BT1 fills one unit on B's entry bar, next
        // to A's AT2; B's second unit stays open to the 13:30 close_all.
        const Tape& other = tapes["t1-two-lots-exit-qty1"];
        if (other.trades.size() == 4) {
            CHECK(other.entries[2] == "B" && other.exits[2] == "BT1");
            CHECK(std::get<3>(other.trades[2]) == lots(1.0));
            CHECK(other.entries[3] == "B" && other.exits[3] == "Close position order");
            CHECK(std::get<0>(other.trades[3]) == std::get<0>(other.trades[2]));
            CHECK(std::get<4>(other.trades[3]) > std::get<4>(other.trades[2]));
        }
        // The same id: A's qty=1 exit AX closes the older A lot and covers the
        // same-bar A add at its own price, two units for a one-unit exit.
        const Tape& same = tapes["t3-same-id-add-cover"];
        if (same.trades.size() == 2) {
            CHECK(same.exits[0] == "AX" && same.exits[1] == "AX");
            CHECK(std::get<4>(same.trades[0]) == std::get<4>(same.trades[1]));
            CHECK(std::get<5>(same.trades[0]) == std::get<5>(same.trades[1]));
            CHECK(std::get<0>(same.trades[1]) == std::get<4>(same.trades[1]));
        }
        // FIFO: B's qty=1 exit BT1 fills on B's entry bar against A's older
        // lot; both units of B stay open to the 13:30 close_all.
        const Tape& fifo = tapes["t4-fifo-other-id-drain"];
        if (fifo.trades.size() == 2) {
            CHECK(fifo.entries[0] == "A" && fifo.exits[0] == "BT1");
            CHECK(std::get<3>(fifo.trades[0]) == lots(1.0));
            CHECK(fifo.entries[1] == "B" && fifo.exits[1] == "Close position order");
            CHECK(std::get<3>(fifo.trades[1]) == lots(2.0));
            CHECK(std::get<0>(fifo.trades[1]) == std::get<4>(fifo.trades[0]));
            CHECK(std::get<4>(fifo.trades[1]) > std::get<4>(fifo.trades[0]));
        }
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
