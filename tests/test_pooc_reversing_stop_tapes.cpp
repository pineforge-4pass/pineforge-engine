/*
 * test_pooc_reversing_stop_tapes.cpp -- R5 lane PAR-ORDERS-2 (item 3).
 *
 * Under process_orders_on_close, a stop entry placed while the book holds the
 * other side, whose stop the placing close has already reached, fills at that
 * close: after a same-bar strategy.close of the held side (corpus probe 96's
 * flip), and as a plain reversal when no close precedes it. R5 lane PAR-ORDERS
 * (H-MEASURE Finding 6b) filled only such an entry placed on a FLAT book there;
 * this one waited for the next open, a bar late on every row.
 *
 * The lane's own synthetic script, probe 96's flip on bar counts, is taped on
 * NYSE:F 15m over the three days of tests/fixtures/session_islastbar (`lab tv`,
 * ws-report-v1, rangeProof covered; tests/fixtures/pooc_reversing_stop, whose
 * README names each tape), and replayed here on the 15m chart and on the 1m
 * bars aggregated to 15m under the bar magnifier:
 *   pa2-i3-flip-s-*  every six bars a reached stop entry of the other side and
 *                    a same-bar strategy.close of the held id; the first flip
 *                    opens a short. The entry's quantity grows by the side it
 *                    was placed against (100, 200, 300, ...), as in probe 96.
 *   pa2-i3-flip-l-*  the same, the first flip opening a long.
 *   pa2-i3-rev-s-*   the reversal alone: no same-bar close.
 *   pa2-i3-flip-unreached-s-*  the stops 5c beyond the close: resting, filled
 *                    later on the bar they are reached -- the control the rule
 *                    must leave alone (it matched before and after).
 * Every closed row is compared field by field -- side, instants, prices,
 * quantity, signals, bar indices -- and TradingView's open trade at the end of
 * the data (its exit signal empty) against the engine's open position.
 *
 * Fail-before, this TU against the lane's previous commit (128 of 178 checks
 * fail): every flip and reversal entry but the first books at the next open
 * (bar j+1, the open's price) where TradingView books the signal bar's close
 * -- 20 of 21 closed rows on each flip tape and every row of each reversal
 * tape (20 against TradingView's 21) -- and the book the data ends on is not
 * TradingView's (flat against 2200 on the flip tapes, short 100 against long
 * 100 on the reversal tapes). The unreached controls match either way.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

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

#ifndef PINEFORGE_PA2_REVERSING_STOP_FIXTURE_DIR
#error "PINEFORGE_PA2_REVERSING_STOP_FIXTURE_DIR must name tests/fixtures/pooc_reversing_stop"
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
    std::ifstream in(std::string(PINEFORGE_PA2_REVERSING_STOP_FIXTURE_DIR) + "/" + slug
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

// floor(x * 100 + 1e-6): the script's cent grid (NYSE:F mintick 0.01).
double cents(double x) { return std::floor(x * 100.0 + 1e-6); }

struct Variant {
    const char* slug;
    bool reached;     // the stop sits on the close's marketable side
    bool same_bar_close;
    bool long_first;
    bool magnifier;
};

// The Pine probe's body (tests/fixtures/pooc_reversing_stop/<slug>/strategy.pine).
class FlipHost final : public source::PineStrategyHost {
public:
    explicit FlipHost(const Variant& v) : v_(v) {
        set_syminfo_session("0930-1600");
        set_syminfo_timezone("America/New_York");
        source::PineStrategyConfig c;
        c.initial_capital = 100000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 100;
        c.pyramiding = 1;
        c.process_orders_on_close = true;
        c.calc_on_order_fills = false;
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
        const int down = v_.long_first ? 5 : 2;
        const int up = v_.long_first ? 2 : 5;
        const bool is_down = k % 6 == down;
        const bool is_up = k % 6 == up;
        const double t = cents(b.close);
        if (is_down) {
            strategy_entry("SE", false, kNaN, (v_.reached ? t + 5.0 : t - 5.0) / 100.0);
            strategy_cancel("LE");
        }
        if (is_up) {
            strategy_entry("LE", true, kNaN, (v_.reached ? t - 5.0 : t + 5.0) / 100.0);
            strategy_cancel("SE");
        }
        if (v_.same_bar_close && is_down && units > 0.0) strategy_close("LE");
        if (v_.same_bar_close && is_up && units < 0.0) strategy_close("SE");
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

// The chart bar (fixture order) whose open is `ms`; -1 when none is.
int chart_bar_of(std::int64_t ms) {
    for (std::size_t i = 0; i < sizeof(kFord15) / sizeof(kFord15[0]); ++i)
        if (kFord15[i].ts == ms) return static_cast<int>(i);
    return -1;
}

// TradingView's exit signal for strategy.close("X") is "Close entry(s) order X".
std::string exit_signal(const FlipHost::Row& r) {
    if (r.exit_id.rfind("__close__", 0) == 0 && r.exit_id.size() > 9)
        return "Close entry(s) order " + r.exit_id.substr(9);
    return r.exit_id;
}

bool near(double a, double b) { return std::fabs(a - b) <= 1e-9; }

void replay(const Variant& v) {
    std::printf("-- %s [%s]\n", v.slug, v.magnifier ? "aggregated 1 -> 15, magnifier"
                                                     : "chart 15 -> 15");
    static const std::vector<Bar> fifteen = feed(kFord15);
    static const std::vector<Bar> one = feed(kFord1m);
    FlipHost host(v);
    if (v.magnifier) {
        host.run(one.data(), static_cast<int>(one.size()), "1", "15", true);
    } else {
        host.run(fifteen.data(), static_cast<int>(fifteen.size()), "15", "15", false);
    }
    CHECK(host.last_error().empty());
    std::vector<TapeTrade> closed;
    const TapeTrade* open_at_end = nullptr;
    const auto tape = read_tape(v.slug);
    for (const TapeTrade& t : tape) {
        if (t.exit_signal.empty()) open_at_end = &t;
        else closed.push_back(t);
    }
    CHECK(!closed.empty());
    const auto rows = host.rows();
    CHECK(rows.size() == closed.size());
    int differing = 0;
    for (std::size_t i = 0; i < rows.size() && i < closed.size(); ++i) {
        const TapeTrade& tv = closed[i];
        const FlipHost::Row& r = rows[i];
        const bool ok = r.is_long == tv.is_long && r.entry_ms == tv.entry_ms
            && r.exit_ms == tv.exit_ms && near(r.entry_price, tv.entry_price)
            && near(r.exit_price, tv.exit_price) && r.qty == tv.qty
            && r.entry_id == tv.entry_signal && exit_signal(r) == tv.exit_signal
            && r.entry_bar == chart_bar_of(tv.entry_ms) && r.exit_bar == chart_bar_of(tv.exit_ms);
        CHECK(ok);
        if (!ok) {
            ++differing;
            std::printf("      row %zu: TradingView %s %s bar %d @%.2f -> bar %d @%.2f %s q%g\n"
                        "              engine      %s %s bar %d @%.2f -> bar %d @%.2f %s q%g\n",
                        i + 1, tv.entry_signal.c_str(), tv.is_long ? "L" : "S",
                        chart_bar_of(tv.entry_ms), tv.entry_price, chart_bar_of(tv.exit_ms),
                        tv.exit_price, tv.exit_signal.c_str(), tv.qty, r.entry_id.c_str(),
                        r.is_long ? "L" : "S", r.entry_bar, r.entry_price, r.exit_bar,
                        r.exit_price, exit_signal(r).c_str(), r.qty);
        }
    }
    // TradingView's open trade at the end of the data is the engine's open
    // position: same side and quantity.
    const double tv_open = open_at_end
        ? (open_at_end->is_long ? open_at_end->qty : -open_at_end->qty) : 0.0;
    CHECK(host.open_units() == tv_open);
    std::printf("      %zu closed rows, %d differ; open at the end %g (TradingView %g)\n",
                rows.size(), differing, host.open_units(), tv_open);
}

}  // namespace

int main() {
    const Variant variants[] = {
        {"pa2-i3-flip-s-chart", true, true, false, false},
        {"pa2-i3-flip-s-mag", true, true, false, true},
        {"pa2-i3-flip-l-chart", true, true, true, false},
        {"pa2-i3-flip-l-mag", true, true, true, true},
        {"pa2-i3-rev-s-chart", true, false, false, false},
        {"pa2-i3-rev-s-mag", true, false, false, true},
        {"pa2-i3-flip-unreached-s-chart", false, true, false, false},
        {"pa2-i3-flip-unreached-s-mag", false, true, false, true},
    };
    for (const Variant& v : variants) replay(v);
    std::printf("\n%s process_orders_on_close reversing stop tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
