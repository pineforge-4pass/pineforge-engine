/*
 * test_whole_lot_tie_tapes.cpp -- R5 lane TAIL-D.
 *
 * A default percent_of_equity (100 %) market entry on whole lots is sized on
 * the signal's equity rounded to ten significant digits. When that rounding
 * ties -- the whole lots it sizes cost more than the equity -- TradingView
 * drops a flat entry and keeps only the closing leg of a reversal, whatever
 * the next open does. The adapter applied the tie only when the next open
 * equalled the signal close and dropped a reversal whole, so a gapped
 * reversal went on to the all-in fill check and was refused: the long was
 * held (jaydeepp095-candle-harry on NYSE:F 1D, 2025-12-16). The equity is the
 * capital plus the realized profit plus the open profit, in floating point:
 * a residue of that sum decides an exact tie (-drift-under / -over below).
 * A running sum of many closed profits carries a residue of its own the
 * engine does not reproduce; where the engine's sum and that sum on the money
 * grid disagree on the tie, the adapter keeps the no-gap rule it had
 * (population probes, cited in resolve_terms).
 *
 * The lane's own synthetic scripts (tests/fixtures/whole_lot_tie, one `lab tv
 * --no-note` export each, NYSE:F 1D, 2025-04-01 .. 2026-05-01, 100 % of
 * equity, 100 % margin): an all-in long entered on 2025-10-13 --
 *   jd-rev-tie-pair     reversed on 12-15 by strategy.entry("Short") and
 *                       strategy.close("Long"); the capital makes the
 *                       signal-close equity a whole-share multiple of the
 *                       close, and the next open gaps up;
 *   jd-rev-notie-pair   the same with one cent more capital (control: no
 *                       tie, TradingView holds the long);
 *   jd-rev-tie-bare     the tie without strategy.close;
 *   jd-rev-tie-gapdown  a tie 0.00003 under the multiple, the next open below
 *                       the close;
 *   jd-rev-tie-nogap    a tie 0.00003 under, the next open equal to the close;
 *   jd-flat-tie-gapdown the flat entry itself on a tie (TradingView drops it);
 * and after a fixed-size trade P (2025-04-04 .. 04-08) whose loss, added to
 * the capital, sums one ulp under (jd-drift-under, 101 shares) or over
 * (jd-drift-over, 102 shares) its cent value, an all-in long entered on
 * 2025-09-02 and reversed by strategy.entry("Short") on 09-04, where that
 * cent value makes the equity the whole shares' exact cost and the next open
 * gaps up: -under is one ulp short of the cost (close-only), -over equals it
 * (no tie; the gapped reversal is refused and the long held).
 * Each row replays TradingView's own tape through the Pine adapter over the
 * tape's bars (bars.inc) and requires every trade the tape closes to be the
 * engine's: entry and exit time, side, price in ticks and quantity.
 *
 * Fail-before (lane report): the tie-pair, -bare, -nogap and -drift-under
 * reversals hold the long; the gapdown reversal fills whole; the flat tie
 * entry fills.
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

#ifndef PINEFORGE_TAIL_D_WHOLE_LOT_TIE_FIXTURE_DIR
#error "PINEFORGE_TAIL_D_WHOLE_LOT_TIE_FIXTURE_DIR must name tests/fixtures/whole_lot_tie"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/whole_lot_tie/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;

// (entry ms, long, entry ticks, quantity, exit ms, exit ticks)
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }

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

std::vector<Row> tape_trades(const std::string& tape) {
    std::ifstream in(std::string(PINEFORGE_TAIL_D_WHOLE_LOT_TIE_FIXTURE_DIR) + "/" + tape
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
        if (cell.size() < 6) continue;
        Row& row = by_number[std::stoi(cell[0])];
        const double price = std::stod(cell[4]);
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(price);
            std::get<3>(row) = std::llround(std::stod(cell[5]));
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(price);
        }
    }
    std::vector<Row> out;
    for (const auto& [number, row] : by_number) out.push_back(row);
    return out;
}

struct Date { unsigned m, d; int y; };

struct Variant {
    const char* tape;
    double capital;
    double prior_qty;     // a fixed-size trade P first, 2025-04-04 .. 04-08 (0: none)
    Date entry_on;        // the all-in long's signal bar
    bool reversal;        // else the flat entry alone
    bool paired_close;    // strategy.close("Long") after the reversal's entry
    Date reverse_on;      // the reversal's signal bar
    Date close_all_on;    // strategy.close_all()
    std::size_t trades;   // tape rows
};

// The probes' bodies (tests/fixtures/whole_lot_tie/<slug>/strategy.pine).
class WholeLotHost final : public source::PineStrategyHost {
public:
    explicit WholeLotHost(const Variant& v) : v_(v) {
        attach_pine_execution_adapter();
        set_syminfo_session("0930-1600");
        set_syminfo_timezone("America/New_York");
        source::PineStrategyConfig c{};
        c.initial_capital = v.capital;
        c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        c.default_qty_value = 100.0;
        c.margin_long = 100.0;
        c.margin_short = 100.0;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", 1.0);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar&) override {
        // A daily bar opens at 09:30 New York, the same calendar date in UTC.
        const std::int64_t day = current_bar_.timestamp / 86'400'000;
        const auto is_day = [day](const Date& d) {
            return day == days_from_civil(d.y, d.m, d.d);
        };
        if (v_.prior_qty > 0.0 && is_day({4, 4, 2025}))
            strategy_entry("P", true, kNaN, kNaN, v_.prior_qty);
        if (v_.prior_qty > 0.0 && is_day({4, 8, 2025})) strategy_close("P");
        if (is_day(v_.entry_on)) strategy_entry("Long", true);
        if (v_.reversal && is_day(v_.reverse_on)) {
            strategy_entry("Short", false);
            if (v_.paired_close) strategy_close("Long");
        }
        if (is_day(v_.close_all_on)) strategy_close_all();
    }

private:
    const Variant& v_;
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kFord1d) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    return bars;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-6s %lld %s @%lld q=%lld -> %lld @%lld\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

void replay(const Variant& v) {
    std::printf("-- %s\n", v.tape);
    static const std::vector<Bar> bars = feed();
    const std::vector<Row> tape = tape_trades(v.tape);
    CHECK(tape.size() == v.trades);
    WholeLotHost host(v);
    host.set_trade_start_time(bars.front().timestamp);
    host.run(bars.data(), static_cast<int>(bars.size()), "1D", "1D", false);
    CHECK(host.last_error().empty());
    std::vector<Row> engine;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        engine.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), std::llround(t.qty),
                            t.exit_time, ticks(t.exit_price));
    }
    CHECK(engine == tape);
    if (engine != tape) {
        show("tape", tape);
        show("engine", engine);
    }
}

}  // namespace

int main() {
    const Variant variants[] = {
        {"jd-rev-tie-pair", 100000.0, 0, {10, 13, 2025}, true, true, {12, 15, 2025},
         {1, 9, 2026}, 1},
        {"jd-rev-notie-pair", 100000.01, 0, {10, 13, 2025}, true, true, {12, 15, 2025},
         {1, 9, 2026}, 1},
        {"jd-rev-tie-bare", 100000.0, 0, {10, 13, 2025}, true, false, {12, 15, 2025},
         {1, 9, 2026}, 1},
        {"jd-rev-tie-gapdown", 100002.03997, 0, {10, 13, 2025}, true, true, {12, 16, 2025},
         {12, 17, 2025}, 1},
        {"jd-rev-tie-nogap", 99994.17997, 0, {10, 13, 2025}, true, true, {12, 9, 2025},
         {12, 11, 2025}, 1},
        {"jd-flat-tie-gapdown", 99994.099997, 0, {10, 13, 2025}, false, false, {1, 1, 2025},
         {10, 15, 2025}, 0},
        {"jd-drift-under", 97102.51, 101, {9, 2, 2025}, true, false, {9, 4, 2025},
         {9, 10, 2025}, 2},
        {"jd-drift-over", 97079.94, 102, {9, 2, 2025}, true, false, {9, 4, 2025},
         {9, 10, 2025}, 2},
    };
    for (const Variant& v : variants) replay(v);
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
