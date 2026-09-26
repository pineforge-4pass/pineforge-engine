/*
 * test_pooc_limit_close_tapes.cpp -- R5 lane PAR-ORDERS-3 (finding 3).
 *
 * Under process_orders_on_close, a limit entry placed by a bar's close
 * calculation is evaluated against that bar's close in the post-calculation
 * pass (flush_pooc_marketable_limit_entry_fills). TradingView fills it at that
 * close when the close's TICK is on its marketable side or on it -- a raw
 * 11.755 close prints 11.76 and fills a sell limit at 11.76 -- and otherwise
 * leaves it resting for the next bar's touch however far the bar's own range
 * reached; with and without calc_on_order_fills. The engine compared the raw
 * close and skipped every limit under calc_on_order_fills, so the 11.755 /
 * 11.76 entry filled a bar late.
 *
 * The lane's own synthetic script, NYSE:F 15m over the three days of
 * tests/fixtures/session_islastbar (`lab tv`, ws-report-v1, rangeProof covered;
 * tests/fixtures/pooc_limit_close, whose README names each tape): at bar j's
 * close calculation, flat, a limit entry at a level the close's tick or the
 * bar's range reaches; the entry is cancelled and the book closed on bar j+2.
 *   pa3-f3-pooc-limit-short*  sell limits: j = 22 (close 11.755, high 11.755)
 *                             at 11.76 -- the tick alone reaches it; j = 27
 *                             (close 11.845, high 11.95) and j = 49 (close
 *                             11.595, high 11.625) -- the range alone; j = 31
 *                             (close 11.855, high 11.87) at 11.86 -- both.
 *   pa3-f3-pooc-limit-long*   buy limits: j = 13 (close 11.655, low 11.655) at
 *                             11.65 and j = 23 (close 11.745, low 11.745) at
 *                             11.74 -- the stored double's nearest tick would
 *                             reach them, the close's tick (11.66, 11.75) does
 *                             not; j = 38 (close 11.805, low 11.79) at 11.79 and
 *                             j = 46 (close 11.62, low 11.61) at 11.61 -- the
 *                             range alone.
 * TradingView fills j = 22 and 31 at their close and every other entry on the
 * next bar, on all four tapes. Every row is compared field by field -- side,
 * instants, prices, quantity, signals, bar indices.
 *
 * Fail-before, this TU against the lane's previous commit (bc5f7a21) and its
 * base (9f7a025c) alike: 4 of 28 checks fail -- both short tapes' j = 22 and
 * j = 31 entries fill on the next bar (rows 1 and 3), with and without
 * calc_on_order_fills; the long tapes agree.
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

#ifndef PINEFORGE_PA3_POOC_LIMIT_FIXTURE_DIR
#error "PINEFORGE_PA3_POOC_LIMIT_FIXTURE_DIR must name tests/fixtures/pooc_limit_close"
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
    std::ifstream in(std::string(PINEFORGE_PA3_POOC_LIMIT_FIXTURE_DIR) + "/" + slug
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

struct Variant {
    const char* slug;
    bool is_long;
    bool coof;
};

// The Pine probe's body (tests/fixtures/pooc_limit_close/<slug>/strategy.pine).
class LimitHost final : public source::PineStrategyHost {
public:
    explicit LimitHost(const Variant& v) : v_(v) {
        set_syminfo_session("0930-1600");
        set_syminfo_timezone("America/New_York");
        source::PineStrategyConfig c;
        c.initial_capital = 100000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 100;
        c.pyramiding = 1;
        c.process_orders_on_close = true;
        c.calc_on_order_fills = v.coof;
        c.slippage = 0;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.01;
        c.margin_long = 100;
        c.margin_short = 100;
        configure_pine_strategy(c);
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        const int k = bar_index_;  // the replay's first bar is the Pine counter's origin
        const double units = signed_position_size();
        static const int kLong[] = {13, 23, 38, 46};
        static const double kLongLevel[] = {11.65, 11.74, 11.79, 11.61};
        static const int kShort[] = {22, 27, 31, 49};
        static const double kShortLevel[] = {11.76, 11.86, 11.86, 11.61};
        const int* js = v_.is_long ? kLong : kShort;
        const double* level = v_.is_long ? kLongLevel : kShortLevel;
        const std::string id = v_.is_long ? "L" : "S";
        for (int c = 0; c < 4; ++c) {
            if (k == js[c] && units == 0.0) strategy_entry(id, v_.is_long, level[c]);
            if (k == js[c] + 2) {
                strategy_cancel(id);
                // codegen spells strategy.close_all() as a close of "".
                if (units != 0.0) strategy_close("");
            }
        }
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

// TradingView's exit signal for a close of "" (strategy.close_all).
std::string exit_signal(const LimitHost::Row& r) {
    return r.exit_id.rfind("__", 0) == 0 ? "Close position order" : r.exit_id;
}

bool near(double a, double b) { return std::fabs(a - b) <= 1e-9; }

void replay(const Variant& v) {
    std::printf("-- %s\n", v.slug);
    static const std::vector<Bar> fifteen = feed(kFord15);
    LimitHost host(v);
    host.run(fifteen.data(), static_cast<int>(fifteen.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    const auto tape = read_tape(v.slug);
    const auto rows = host.rows();
    CHECK(tape.size() == 4);
    CHECK(rows.size() == tape.size());
    int differing = 0;
    for (std::size_t i = 0; i < rows.size() && i < tape.size(); ++i) {
        const TapeTrade& tv = tape[i];
        const LimitHost::Row& r = rows[i];
        const bool ok = r.is_long == tv.is_long && r.entry_ms == tv.entry_ms
            && r.exit_ms == tv.exit_ms && near(r.entry_price, tv.entry_price)
            && near(r.exit_price, tv.exit_price) && r.qty == tv.qty
            && r.entry_id == tv.entry_signal && exit_signal(r) == tv.exit_signal
            && r.entry_bar == chart_bar_of(tv.entry_ms) && r.exit_bar == chart_bar_of(tv.exit_ms);
        CHECK(ok);
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
    std::printf("      %zu rows compared, %d differ\n", rows.size(), differing);
}

}  // namespace

int main() {
    const Variant variants[] = {
        {"pa3-f3-pooc-limit-long", true, false},
        {"pa3-f3-pooc-limit-long-coof", true, true},
        {"pa3-f3-pooc-limit-short", false, false},
        {"pa3-f3-pooc-limit-short-coof", false, true},
    };
    for (const Variant& v : variants) replay(v);
    std::printf("\n%s process_orders_on_close limit close tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
