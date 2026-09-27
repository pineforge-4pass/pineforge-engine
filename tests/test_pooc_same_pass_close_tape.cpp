/*
 * test_pooc_same_pass_close_tape.cpp — lane W8A-SIGSTATE-1, rule R2.
 *
 * Under process_orders_on_close, a strategy.close_all() or a whole-position
 * strategy.close(id) the script issues before a strategy.entry in the same
 * pass settles before that entry. A same-side entry is judged against the
 * position the close flattens: its margin is the resulting position,
 * (held + own) x close x pointvalue x margin %, and it is dropped when that
 * exceeds the equity. An opposite-side entry is not charged for the held
 * lots, and an admitted entry opens after the close as a fresh position, so
 * pyramiding=0 does not block it. A same-side entry the script issued in the
 * same pass before the close survives it too, when the cap admitted it at its
 * call. The adapter charged the entry for its own quantity only, dropped it
 * as a same-side add after a strategy.close(id) under pyramiding=0, and
 * cancelled every same-side market entry of the pass when a queued
 * strategy.close(id) flattened the position.
 *
 * The scraped pf-probe-concord-frangestate (CME_MINI:NQ1! 15) closes its
 * position and re-enters on the same side in one pass: TradingView drops
 * the entry on 15 of those 22 bars, where (held + qty) x close x 20 exceeds
 * the equity, and the engine re-entered on all 22.
 *
 * One lab tv export (tests/fixtures/pooc_same_pass_close/
 * w8a-pooc-sameside-closeall, BINANCE:ETHUSDT.P 15) replayed through the
 * Pine adapter under the configuration its generated constructor declares,
 * over the corpus 15m bars of bars.inc (2025-04-01 12:00 .. 04-09 00:00 UTC,
 * each arm once): every trade must be the engine's -- entry id, entry and
 * exit time, side, price and quantity -- and the rule is read off
 * TradingView's rows. Each day opens H worth 60 % of the equity; one bar
 * later the script closes it and places one entry in the same pass:
 *
 *   A (Mon)  close_all, long 50 %: dropped;
 *   B (Tue)  close_all, long 30 %: fills after the close;
 *   C (Wed)  close_all, short 50 %: fills, the held long is not charged;
 *   D (Thu)  close("H"), long 50 %: dropped;
 *   E (Fri)  close_all, short 50 % over a short H: dropped;
 *   V (Sat)  close_all, long (equity - held x (avg + close) / 2) / close:
 *            fills with the close below the average, the held lots valued
 *            at the close;
 *   D2 (Sun) close("H"), long 30 %: fills after the close.
 *
 * A second export (tests/fixtures/pooc_same_pass_close/
 * w8a-pooc-close-reentry-pyr2, BINANCE:ETHUSDT.P 15, pyramiding=2, fixed 1)
 * over the same bars: each day enters L at 10:00; at 10:15 it calls
 * close("L") then entry("L_add") on even days and the two in the other order
 * on odd days; TradingView closes L and opens L_add at that close on every
 * day, and holds L_add to the 11:00 cleanup.
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
#include <utility>
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

#ifndef PINEFORGE_POOC_SAME_PASS_CLOSE_FIXTURE_DIR
#error "PINEFORGE_POOC_SAME_PASS_CLOSE_FIXTURE_DIR must name tests/fixtures/pooc_same_pass_close"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/pooc_same_pass_close/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:ETHUSDT.P quantity step and price tick.
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;
constexpr std::int64_t kDayMs = 86'400'000;

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry id, entry ms, long, entry price, quantity, exit ms, exit price).
using Row = std::tuple<std::string, std::int64_t, bool, long long, long long, std::int64_t,
                       long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }
bool on_grid(double value, double step) {
    return std::abs(value / step - static_cast<double>(std::llround(value / step))) < 1e-6;
}

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::int64_t utc(int y, unsigned m, unsigned d, int hour, int minute) {
    return ((days_from_civil(y, m, d) * 24 + hour) * 60 + minute) * 60'000;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    return utc(y, static_cast<unsigned>(mo), static_cast<unsigned>(d), h - 8, mi);
}

// TradingView's trades of `slug` entered before `end_ms`, in trade order.
std::vector<Row> tape_trades(const std::string& slug, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_POOC_SAME_PASS_CLOSE_FIXTURE_DIR) + "/" + slug
                     + "/tv_trades.csv");
    std::map<int, Row> by_number;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        // Trade number, Type, Date and time, Signal, Price USDT, Size (qty), ...
        if (cell.size() < 6) continue;
        Row& row = by_number[std::stoi(cell[0])];
        const double price = std::stod(cell[4]);
        CHECK(on_grid(price, kTick));
        if (cell[1].rfind("Entry", 0) == 0) {
            const double qty = std::stod(cell[5]);
            CHECK(on_grid(qty, kLot));
            std::get<0>(row) = cell[3];
            std::get<1>(row) = tape_ms(cell[2]);
            std::get<2>(row) = cell[1] == "Entry long";
            std::get<3>(row) = ticks(price);
            std::get<4>(row) = lots(qty);
        } else {
            std::get<5>(row) = tape_ms(cell[2]);
            std::get<6>(row) = ticks(price);
        }
    }
    std::vector<Row> out;
    for (const auto& [number, row] : by_number)
        if (std::get<1>(row) < end_ms) out.push_back(row);
    return out;
}

// w8a-pooc-sameside-closeall as its generated TU lowers it
// (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.process_orders_on_close = true;
        cfg.initial_capital = 10000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.pyramiding = 0;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        cfg.margin_long = 100.0;
        cfg.margin_short = 100.0;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const double close = current_bar_.close;
        // dayofweek(time, "UTC"): 1 = Sunday .. 7 = Saturday; 1970-01-01
        // was a Thursday.
        const int dow = static_cast<int>((t / kDayMs + 4) % 7) + 1;
        const int hm = static_cast<int>(t % kDayMs / 60'000);
        const bool live = t >= utc(2025, 4, 2, 0, 0) && t < utc(2026, 4, 30, 0, 0);
        const double eqv = current_equity() + open_profit(close);
        const auto steps = [&](double money) {
            return std::floor(money / close / 0.001) * 0.001;
        };
        const double q60 = steps(0.6 * eqv);
        const double q50 = steps(0.5 * eqv);
        const double q30 = steps(0.3 * eqv);
        const double position = signed_position_size();
        const double held = std::abs(position);
        const double mid = ((position == 0.0 ? kNaN : position_entry_price_) + close) / 2;
        const double qv = steps(eqv - held * mid);
        const bool sat = dow == 7;
        const bool open_bar = live && hm == 600;
        const bool act_bar = live && ((!sat && hm == 615) || (sat && hm == 960));
        const bool flat_bar = live && ((!sat && hm == 660) || (sat && hm == 1020));
        const auto entry = [&](const char* id, bool is_long, double qty) {
            strategy_entry(id, is_long, kNaN, kNaN, qty, id, "", 0, -1);
        };
        const auto close_all = [&](const char* comment) {
            strategy_close("", comment, kNaN, kNaN, false);
        };
        if (open_bar && position == 0.0) entry("H", dow != 6, q60);
        if (act_bar && position != 0.0) {
            if (dow == 2) {
                close_all("CA");
                entry("A", true, q50);
            } else if (dow == 3) {
                close_all("CB");
                entry("B", true, q30);
            } else if (dow == 4) {
                close_all("CC");
                entry("C", false, q50);
            } else if (dow == 5) {
                strategy_close("H", "CD", kNaN, kNaN, false, 1);
                entry("D", true, q50);
            } else if (dow == 6) {
                close_all("CE");
                entry("E", false, q50);
            } else if (dow == 1) {
                strategy_close("H", "CD2", kNaN, kNaN, false, 2);
                entry("D2", true, q30);
            } else if (sat) {
                close_all("CV");
                entry("V", true, qv);
            }
        }
        if (flat_bar && signed_position_size() != 0.0) close_all("X");
    }
};

// w8a-pooc-close-reentry-pyr2 as its generated TU lowers it.
class ReentryHost final : public source::PineStrategyHost {
public:
    ReentryHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.process_orders_on_close = true;
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.pyramiding = 2;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const int hm = static_cast<int>(t % kDayMs / 60'000);
        const bool live = t >= utc(2025, 4, 2, 0, 0) && t < utc(2025, 4, 12, 0, 0);
        const auto civil_day = [&] {
            // dayofmonth(time, "UTC")
            const std::int64_t z = t / kDayMs + 719468;
            const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
            const std::int64_t doe = z - era * 146097;
            const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const std::int64_t mp = (5 * doy + 2) / 153;
            return static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
        };
        const bool p_day = civil_day() % 2 == 0;
        const auto entry = [&](const char* id) {
            strategy_entry(id, true, kNaN, kNaN, kNaN, "");
        };
        if (live && hm == 600 && signed_position_size() == 0.0) entry("L");
        if (live && hm == 615 && signed_position_size() > 0.0) {
            if (p_day) {
                strategy_close("L", "", kNaN, kNaN, false, 1);
                entry("L_add");
            } else {
                entry("L_add");
                strategy_close("L", "", kNaN, kNaN, false, 2);
            }
        }
        if (live && hm == 660 && signed_position_size() != 0.0)
            strategy_close("", "", kNaN, kNaN, false);
    }
};

template <typename Host>
std::vector<Row> run(std::string& error) {
    Host host;
    // The bar before each tape's first order's bar (placed at the 04-02 10:00 close).
    host.set_trade_start_time(utc(2025, 4, 2, 9, 45));
    std::vector<Bar> bars;
    for (const FeedBar& row : kEth15) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    error = host.last_error();
    std::vector<Row> out;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        CHECK(on_grid(t.entry_price, kTick));
        CHECK(on_grid(t.exit_price, kTick));
        CHECK(on_grid(t.qty, kLot));
        out.emplace_back(t.entry_id, t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty),
                         t.exit_time, ticks(t.exit_price));
    }
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %-3s %lld %s @%lld ticks q=%lld lots -> %lld @%lld ticks\n", tag,
                    std::get<0>(r).c_str(), static_cast<long long>(std::get<1>(r)),
                    std::get<2>(r) ? "long" : "short", std::get<3>(r), std::get<4>(r),
                    static_cast<long long>(std::get<5>(r)), std::get<6>(r));
}

const Row* row_of(const std::vector<Row>& rows, const std::string& id) {
    for (const Row& r : rows)
        if (std::get<0>(r) == id) return &r;
    return nullptr;
}

// The H trade opened on 2025-04-<day>.
const Row* held_on(const std::vector<Row>& rows, unsigned day) {
    for (const Row& r : rows)
        if (std::get<0>(r) == "H" && std::get<1>(r) == utc(2025, 4, day, 10, 0)) return &r;
    return nullptr;
}

}  // namespace

int main() {
    const std::int64_t end_ms = kEth15[sizeof(kEth15) / sizeof(kEth15[0]) - 1].ts;
    const std::vector<Row> tape = tape_trades("w8a-pooc-sameside-closeall", end_ms);
    CHECK(tape.size() == 11);

    std::printf("-- w8a-pooc-sameside-closeall\n");
    std::string error;
    const std::vector<Row> engine = run<ProbeHost>(error);
    CHECK(error.empty());
    CHECK(engine == tape);
    if (engine != tape) {
        show("tape", tape);
        show("engine", engine);
    }

    // The rule, read off TradingView's rows.
    std::printf("-- the rule, on the tape\n");
    // Every H is closed at the action bar's close; the entry of that pass,
    // when TradingView takes it, opens there too.
    for (const unsigned day : {2u, 3u, 4u, 6u, 7u, 8u}) {
        const Row* h = held_on(tape, day);
        CHECK(h && std::get<5>(*h) == utc(2025, 4, day, 10, 15));
    }
    // Same side, over the equity: A (close_all, 60 % + 50 %), D
    // (close("H"), the same) and E (the short mirror) are dropped.
    CHECK(!row_of(tape, "A") && !row_of(tape, "D") && !row_of(tape, "E"));
    // Same side, within the equity: B and D2 open at the close price, D2
    // after a strategy.close(id) under pyramiding=0.
    for (const auto& [id, day] : {std::pair<const char*, unsigned>{"B", 8}, {"D2", 6}}) {
        const Row* r = row_of(tape, id);
        const Row* h = held_on(tape, day);
        CHECK(r && h && std::get<2>(*r) && std::get<1>(*r) == std::get<5>(*h)
              && std::get<3>(*r) == std::get<6>(*h));
    }
    // Opposite side: C (50 %) is not charged for the held long.
    const Row* c = row_of(tape, "C");
    CHECK(c && !std::get<2>(*c) && std::get<1>(*c) == utc(2025, 4, 2, 10, 15));
    // V: its quantity overshoots the equity exactly when the held lots are
    // valued at the close and the close is above their average; on 04-05 the
    // close (V's entry price) is below H's entry price and V fills.
    const Row* v = row_of(tape, "V");
    const Row* h5 = held_on(tape, 5);
    CHECK(v && h5 && std::get<1>(*v) == utc(2025, 4, 5, 16, 0)
          && std::get<3>(*v) < std::get<3>(*h5));

    std::printf("-- w8a-pooc-close-reentry-pyr2\n");
    const std::vector<Row> reentry_tape = tape_trades("w8a-pooc-close-reentry-pyr2", end_ms);
    CHECK(reentry_tape.size() == 14);
    const std::vector<Row> reentry = run<ReentryHost>(error);
    CHECK(error.empty());
    CHECK(reentry == reentry_tape);
    if (reentry != reentry_tape) {
        show("tape", reentry_tape);
        show("engine", reentry);
    }
    // The rule, read off TradingView's rows: on every day, whichever call came
    // first, L closes at 10:15 and L_add opens at that close, held to 11:00.
    for (unsigned day = 2; day <= 8; ++day) {
        const Row* l = nullptr;
        const Row* add = nullptr;
        for (const Row& r : reentry_tape) {
            if (std::get<1>(r) == utc(2025, 4, day, 10, 0) && std::get<0>(r) == "L") l = &r;
            if (std::get<1>(r) == utc(2025, 4, day, 10, 15) && std::get<0>(r) == "L_add") add = &r;
        }
        CHECK(l && add && std::get<5>(*l) == utc(2025, 4, day, 10, 15)
              && std::get<3>(*add) == std::get<6>(*l)
              && std::get<5>(*add) == utc(2025, 4, day, 11, 0));
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
