/*
 * test_flat_coof_exit_tapes.cpp -- R5 lane PAR-ORDERS-2 (item 1).
 *
 * A strategy.exit placed in a calc_on_order_fills recalculation on a FLAT book
 * -- the recalculation a take-profit or stop-loss fill starts mid-way along the
 * bar's first leg. The Pine adapter classifies the exit's stop at its birth
 * against the book (exit(): an already-marketable stop goes to the next open
 * as a market close, or with a quantity to the next waypoint), and a flat book
 * has no side: it read a long's protective stop below the price as a short's
 * buy stop, already reached. In the live-state battery (reversals seed
 * 3246599) the delayed leg then reached the kernel's current execution as a
 * cohort-bound close, which failed the run until the kernel refused it
 * (tests/test_native_current_cohort_refusal.cpp), and closed the long at the
 * next open though neither of its levels was touched.
 *
 * The lane's own synthetic scripts, NYSE:F 15m over the three days of
 * tests/fixtures/session_islastbar (`lab tv`, ws-report-v1, rangeProof covered;
 * tests/fixtures/flat_coof_exit, whose README names each tape). Market entry E
 * fills at bar j's open and its exit TX sits inside that bar's first leg (a
 * limit on a high-first bar, a stop on a low-first one), so TX flattens the
 * book mid-leg (cycles j = 2, 30, 41), and the recalculation that fill starts
 *   pa2-i1-pend-xf-pooc       places a buy limit entry L 2c below and its
 *                             bracket XF 50c either side (never touched);
 *   pa2-i1-pend-xs-crossed-pooc  places a sell limit entry S 2c above and its
 *                             exit XS with the stop 50c BELOW the price --
 *                             crossed as soon as S is open (without the
 *                             kernel's refusal this run failed);
 *   pa2-i1-xf-* / xs-*        places an exit for an id with no order yet, a
 *                             market entry M, and enters the id on bar j+1
 *                             (qty 100 or the whole position; with and without
 *                             process_orders_on_close);
 *   pa2-i1-xf-tight-qty-*     the same with a bracket 3c either side, reached
 *                             once L is open -- and once without
 *                             calc_on_order_fills, the exit placed at bar
 *                             j-1's close calculation.
 *
 * What TradingView books, and what the rows pin:
 *   - A pending parent's protective bracket rests (pend-xf, 3 of 3 cycles):
 *     rows 1-5 exact. Row 6 is a RECORDED divergence of the limit entry itself
 *     (TradingView fills L at 11.65, this engine at 11.70), not of its exit.
 *   - A pending parent's crossed stop exits at the parent's own fill, on its
 *     bar (pend-xs-crossed, 3 of 3): this engine books it on the next bar
 *     (row 2) or at the stop's own level, far from the market (rows 4, 6).
 *     RECORDED; every run completes.
 *   - An exit placed while its id has no order at all is never active on
 *     TradingView: the id's trade runs to the close_all even with a 3c bracket
 *     (xf-tight-qty), and without calc_on_order_fills this engine agrees
 *     (xf-tight-qty-nocoof: exact). Inside a recalculation it does not: an
 *     exit with a quantity is re-targeted to the next waypoint and closes the
 *     trade a bar after its entry (xf-qty rows 3 and 6 / 6, xs-qty rows 3 and
 *     9), and a reached bracket fills (xf-tight-qty rows 3, 6, 9). RECORDED.
 * Every other row is exact: side, instants, prices, quantity, signals, bars.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
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

#ifndef PINEFORGE_PA2_FLAT_COOF_FIXTURE_DIR
#error "PINEFORGE_PA2_FLAT_COOF_FIXTURE_DIR must name tests/fixtures/flat_coof_exit"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/session_islastbar/bars.inc"

constexpr std::int64_t kMinute = 60'000;
const double kNaN = std::numeric_limits<double>::quiet_NaN();

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// "YYYY-MM-DD HH:MM" in the tape's UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

struct TapeTrade {
    bool is_long = true;
    std::int64_t entry_ms = 0, exit_ms = 0;
    double entry_price = 0.0, exit_price = 0.0, qty = 0.0;
    std::string entry_signal, exit_signal;
};

std::vector<TapeTrade> read_tape(const std::string& slug) {
    std::ifstream in(std::string(PINEFORGE_PA2_FLAT_COOF_FIXTURE_DIR) + "/" + slug
                     + "/tv_trades.csv");
    std::vector<TapeTrade> trades;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 17) continue;
        const std::size_t n = static_cast<std::size_t>(std::stoi(cell[0]));
        if (trades.size() < n) trades.resize(n);
        TapeTrade& t = trades[n - 1];
        const bool entry = cell[1].rfind("Entry", 0) == 0;
        t.is_long = cell[1].find("long") != std::string::npos;
        (entry ? t.entry_ms : t.exit_ms) = tape_ms(cell[2]);
        (entry ? t.entry_signal : t.exit_signal) = cell[3];
        (entry ? t.entry_price : t.exit_price) = std::stod(cell[4]);
        t.qty = std::stod(cell[5]);
    }
    return trades;
}

// floor(x * 100 + 1e-6): the scripts' cent grid (NYSE:F mintick 0.01).
double cents(double x) { return std::floor(x * 100.0 + 1e-6); }

enum class Shape {
    PendingBracket,   // pend-xf
    PendingCrossed,   // pend-xs-crossed
    NoOrderLong,      // xf-*: exit for L with no order yet
    NoOrderShort,     // xs-*: exit for S with no order yet
    NoOrderTight,     // xf-tight-qty: the bracket 3c either side
    NoOrderTightBar,  // xf-tight-qty-nocoof: placed at bar j-1's close
};

struct Variant {
    const char* slug;
    Shape shape;
    bool qty;         // the exit carries qty=100
    bool pooc;
    int pyramiding;
    unsigned recorded;  // bit i+1: row i+1 is a RECORDED divergence
};

// The Pine probes' bodies (tests/fixtures/flat_coof_exit/<slug>/strategy.pine).
class FlatExitHost final : public source::PineStrategyHost {
public:
    explicit FlatExitHost(const Variant& v) : v_(v) {
        set_syminfo_session("0930-1600");
        set_syminfo_timezone("America/New_York");
        source::PineStrategyConfig c;
        c.initial_capital = 100000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 100;
        c.pyramiding = v.pyramiding;
        c.process_orders_on_close = v.pooc;
        c.calc_on_order_fills = v.shape != Shape::NoOrderTightBar;
        c.slippage = 0;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.01;
        c.margin_long = 100;
        c.margin_short = 100;
        configure_pine_strategy(c);
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar& b) override {
        const int k = bar_index_;  // the replay's first bar is the Pine counter's origin
        const double units = signed_position_size();
        const int n = trade_count();
        const bool just_flat = units == 0.0 && n > 0 && closed_trade_exit_id(n - 1) == "TX"
            && closed_trade_exit_bar_index(n - 1) == pine_bar_index();
        const double lvl = k == 1 ? 11.46 : k == 29 ? 11.85 : k == 40 ? 11.66 : kNaN;
        const double t = cents(b.close);
        const double qty = v_.qty ? 100.0 : kNaN;
        if (!std::isnan(lvl) && units == 0.0) {
            strategy_entry("E", true);
            if (k == 40) strategy_exit("TX", "E", kNaN, lvl);
            else strategy_exit("TX", "E", lvl, kNaN);
            if (v_.shape == Shape::NoOrderTightBar)
                strategy_exit("XF", "L", (t + 3.0) / 100.0, (t - 3.0) / 100.0, kNaN, kNaN, kNaN,
                              100.0, {}, 100.0);
        }
        if (just_flat) {
            switch (v_.shape) {
            case Shape::PendingBracket:
                strategy_entry("L", true, (t - 2.0) / 100.0);
                strategy_exit("XF", "L", (t + 50.0) / 100.0, (t - 50.0) / 100.0);
                break;
            case Shape::PendingCrossed:
                strategy_entry("S", false, (t + 2.0) / 100.0);
                strategy_exit("XS", "S", (t - 100.0) / 100.0, (t - 50.0) / 100.0);
                break;
            case Shape::NoOrderLong:
                strategy_exit("XF", "L", (t + 50.0) / 100.0, (t - 50.0) / 100.0, kNaN, kNaN,
                              kNaN, 100.0, {}, qty);
                strategy_entry("M", true);
                break;
            case Shape::NoOrderShort:
                strategy_exit("XS", "S", (t - 100.0) / 100.0, (t - 50.0) / 100.0, kNaN, kNaN,
                              kNaN, 100.0, {}, qty);
                strategy_entry("M", true);
                break;
            case Shape::NoOrderTight:
                strategy_exit("XF", "L", (t + 3.0) / 100.0, (t - 3.0) / 100.0, kNaN, kNaN,
                              kNaN, 100.0, {}, 100.0);
                strategy_entry("M", true);
                break;
            case Shape::NoOrderTightBar:
                break;
            }
        }
        const bool reenter = k == 3 || k == 31 || k == 42;
        if (v_.shape == Shape::NoOrderTightBar) {
            if (reenter && units == 0.0) strategy_entry("L", true);
        } else if (v_.shape != Shape::PendingBracket && v_.shape != Shape::PendingCrossed
                   && reenter && units != 0.0) {
            strategy_close("M");
            const bool long_id = v_.shape != Shape::NoOrderShort;
            strategy_entry(long_id ? "L" : "S", long_id);
        }
        if ((k == 5 || k == 33 || k == 44) && units != 0.0) strategy_close_all();
    }

    struct Row {
        bool is_long = true;
        std::int64_t entry_ms = 0, exit_ms = 0;
        double entry_price = 0.0, exit_price = 0.0, qty = 0.0;
        int entry_bar = 0, exit_bar = 0;
        std::string entry_id, exit_id;
    };
    std::vector<Row> rows() const {
        std::vector<Row> out;
        for (int i = 0; i < trade_count(); ++i) {
            Row r;
            r.is_long = trades_[static_cast<std::size_t>(i)].is_long;
            r.entry_ms = closed_trade_entry_time(i);
            r.exit_ms = closed_trade_exit_time(i);
            r.entry_price = closed_trade_entry_price(i);
            r.exit_price = closed_trade_exit_price(i);
            r.qty = closed_trade_size(i);
            r.entry_bar = closed_trade_entry_bar_index(i);
            r.exit_bar = closed_trade_exit_bar_index(i);
            r.entry_id = closed_trade_entry_id(i);
            r.exit_id = closed_trade_exit_id(i);
            out.push_back(r);
        }
        return out;
    }
    double open_units() const { return physical_position().signed_units; }

private:
    Variant v_;
};

template <std::size_t N>
std::vector<Bar> feed(const FeedBar (&rows)[N]) {
    std::vector<Bar> bars;
    for (const FeedBar& row : rows) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    return bars;
}

int chart_bar_of(std::int64_t ms) {
    for (std::size_t i = 0; i < sizeof(kFord15) / sizeof(kFord15[0]); ++i)
        if (kFord15[i].ts == ms) return static_cast<int>(i);
    return -1;
}

// TradingView's exit signals: "Close entry(s) order X" for strategy.close("X"),
// "Close position order" for strategy.close_all.
std::string exit_signal(const FlatExitHost::Row& r) {
    if (r.exit_id.rfind("__close__", 0) == 0 && r.exit_id.size() > 9)
        return "Close entry(s) order " + r.exit_id.substr(9);
    return r.exit_id.rfind("__", 0) == 0 ? "Close position order" : r.exit_id;
}

bool near(double a, double b) { return std::fabs(a - b) <= 1e-9; }

void replay(const Variant& v) {
    std::printf("-- %s\n", v.slug);
    static const std::vector<Bar> fifteen = feed(kFord15);
    FlatExitHost host(v);
    host.run(fifteen.data(), static_cast<int>(fifteen.size()), "15", "15", false);
    // Every run completes: the pending crossed stop used to end it.
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("      error: %s\n", host.last_error().c_str());
    const auto tape = read_tape(v.slug);
    const auto rows = host.rows();
    CHECK(!tape.empty());
    CHECK(rows.size() == tape.size());
    CHECK(host.open_units() == 0.0);
    int differing = 0;
    for (std::size_t i = 0; i < rows.size() && i < tape.size(); ++i) {
        const TapeTrade& tv = tape[i];
        const FlatExitHost::Row& r = rows[i];
        const bool ok = r.is_long == tv.is_long && r.entry_ms == tv.entry_ms
            && r.exit_ms == tv.exit_ms && near(r.entry_price, tv.entry_price)
            && near(r.exit_price, tv.exit_price) && r.qty == tv.qty
            && r.entry_id == tv.entry_signal && exit_signal(r) == tv.exit_signal
            && r.entry_bar == chart_bar_of(tv.entry_ms) && r.exit_bar == chart_bar_of(tv.exit_ms);
        const bool divergence = (v.recorded & (1u << (i + 1))) != 0;
        CHECK(ok != divergence);
        if (divergence) std::printf("      row %zu: RECORDED divergence\n", i + 1);
        if (!ok) {
            ++differing;
            std::printf("      row %zu: TradingView %s %s bar %d @%.2f -> bar %d @%.2f %s\n"
                        "              engine      %s %s bar %d @%.2f -> bar %d @%.2f %s\n",
                        i + 1, tv.entry_signal.c_str(), tv.is_long ? "L" : "S",
                        chart_bar_of(tv.entry_ms), tv.entry_price, chart_bar_of(tv.exit_ms),
                        tv.exit_price, tv.exit_signal.c_str(), r.entry_id.c_str(),
                        r.is_long ? "L" : "S", r.entry_bar, r.entry_price, r.exit_bar,
                        r.exit_price, exit_signal(r).c_str());
        }
    }
    std::printf("      %zu rows, %d differ\n", rows.size(), differing);
}

unsigned rows(std::initializer_list<int> list) {
    unsigned mask = 0;
    for (const int row : list) mask |= 1u << row;
    return mask;
}

}  // namespace

int main() {
    const Variant variants[] = {
        {"pa2-i1-pend-xf-pooc", Shape::PendingBracket, false, true, 1, rows({6})},
        {"pa2-i1-pend-xs-crossed-pooc", Shape::PendingCrossed, false, true, 1, rows({2, 4, 6})},
        {"pa2-i1-xf-qty-pooc", Shape::NoOrderLong, true, true, 1, rows({3, 6})},
        {"pa2-i1-xf-qty-pyr2", Shape::NoOrderLong, true, false, 2, rows({6})},
        {"pa2-i1-xf-dyn-pooc", Shape::NoOrderLong, false, true, 1, 0u},
        {"pa2-i1-xs-dyn-pooc", Shape::NoOrderShort, false, true, 1, 0u},
        {"pa2-i1-xs-dyn", Shape::NoOrderShort, false, false, 1, 0u},
        {"pa2-i1-xs-qty-pooc", Shape::NoOrderShort, true, true, 1, rows({3, 9})},
        {"pa2-i1-xs-qty", Shape::NoOrderShort, true, false, 1, rows({3, 9})},
        {"pa2-i1-xf-tight-qty-pooc", Shape::NoOrderTight, true, true, 1, rows({3, 6, 9})},
        {"pa2-i1-xf-tight-qty-nocoof-pooc", Shape::NoOrderTightBar, true, true, 1, 0u},
    };
    for (const Variant& v : variants) replay(v);
    std::printf("\n%s flat recalculation exit tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
