/*
 * test_pooc_stop_reentry_bracket_tapes.cpp -- R5 lane INT26 (round 2).
 *
 * Under process_orders_on_close, a stop entry the script cancels and places
 * again with its strategy.exit bracket at each flat signal close fills on the
 * next bar, and so does the bracket's limit; that bar closes flat, and its
 * close places the entry again, its stop AT the close, which has reached it,
 * with a fresh bracket of the same exit id. R5 lane PAR-ORDERS (H-MEASURE
 * Finding 6b) fills that re-entry at the close, as TradingView does, so the
 * fresh bracket is working when the next bar opens. The first bracket's limit
 * was a retained leg the pre-script drain settled after that bar's receipt
 * read; the adapter observed its terminal receipt only at the next bar's open,
 * and its sibling cancel -- every working leg of the same exit id, from_entry
 * and origin -- withdrew the fresh bracket too, so the re-entry never exited.
 * That is how INT26's gate sweep lost trade 275 of
 * scrapper:data-BINANCE-BTCUSDT/standard/vasudevshenoy-manoj-betrayed-me
 * (BTCUSDT 15m, 2025-08-03 10:00 UTC): 820 rows against TradingView's 821.
 *
 * The shape, synthetic, taped on NYSE:F 15m over the three days of
 * tests/fixtures/session_islastbar (`lab tv`, ws-report-v1, rangeProof covered;
 * tests/fixtures/pooc_stop_reentry_bracket, whose README names each tape): long
 * on bars 39-41 (the fresh bracket's limit on bar 42), short on bars 44-46 (the
 * limit on bar 49). TradingView books the same four rows on the chart and under
 * the bar magnifier. Every row is compared field by field -- side, instants,
 * prices, quantity, signals, bar indices -- and the book the replay ends on,
 * its working bracket legs included.
 *
 *   int26-reentry-bracket-chart  replayed on the 15m chart: all four rows, and
 *                    a flat book. The short is the leaver's shape; the long,
 *                    whose first limit is read before the body, is a control
 *                    the fix leaves alone.
 *   int26-reentry-bracket-mag    replayed on the 1m bars aggregated to 15m
 *                    under the magnifier: rows 1-3. Row 4 is a RECORDED
 *                    divergence, asserted, so a fix flips it deliberately:
 *                    there the drain leaves the short's retained limit to the
 *                    flush below the body, the body still reads a short book
 *                    at bar 46's close, and the re-entry is never placed (3
 *                    rows against TradingView's 4, both books flat). It is the
 *                    same with or without this lane's fix.
 *   int26-reentry-noexit-chart   the control, on the chart: the same script
 *                    but the short's re-entry places no strategy.exit, so it
 *                    is still open when the data ends, as TradingView's row 4
 *                    is. The drain submits the short bracket's legs one at a
 *                    time, executing each: its stop is accepted after its
 *                    limit filled, and it is still that filled order's leg --
 *                    the late read withdraws it, and no bracket leg is left
 *                    working.
 *
 * Fail-before, this TU against the lane's previous head ebe67484 (2 of 27
 * checks fail): on the chart the short re-entry loses its fresh bracket at bar
 * 47's open and is still open when the data ends -- 3 closed rows against
 * TradingView's 4, short 100 against flat. Against a rule that kept every leg
 * accepted after the fill (1 of 28 fail): the control's stop stays working
 * under the re-entry that has no bracket of its own.
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

#ifndef PINEFORGE_INT26_REENTRY_FIXTURE_DIR
#error "PINEFORGE_INT26_REENTRY_FIXTURE_DIR must name tests/fixtures/pooc_stop_reentry_bracket"
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
    std::ifstream in(std::string(PINEFORGE_INT26_REENTRY_FIXTURE_DIR) + "/" + slug
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
    bool magnifier;
    // Closed rows of TradingView's tape the replay books, from the first: all
    // of them, or three of four where row 4 is the recorded magnified
    // divergence.
    std::size_t booked;
    bool short_reentry_exit;  // the short's re-entry on bar 46 places its own bracket
};

// The Pine probe's body (tests/fixtures/pooc_stop_reentry_bracket/<slug>/strategy.pine).
class ReentryHost final : public source::PineStrategyHost {
public:
    explicit ReentryHost(const Variant& v) : v_(v) {
        // As the generated strategy does: the retained-parent ordering of the
        // cancelled-and-placed-again entry is the attached adapter's.
        attach_pine_execution_adapter();
        set_syminfo_session("0930-1600");
        set_syminfo_timezone("America/New_York");
        source::PineStrategyConfig c;
        c.initial_capital = 100000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 100;
        c.pyramiding = 0;
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
        if (signed_position_size() != 0.0) return;
        const double t = cents(b.close);
        // Three signal closes each: the stop 10c beyond the close, 1c beyond
        // it, then AT it; each cancels the pending entry and places it again.
        if (k >= 39 && k <= 41) {
            strategy_cancel("L");
            strategy_entry("L", true, kNaN, (k == 39 ? t + 10.0 : k == 40 ? t + 1.0 : t) / 100.0);
            strategy_exit("LX", "L", (t + 3.0) / 100.0, (t - 10.0) / 100.0);
        }
        if (k >= 44 && k <= 46) {
            strategy_cancel("S");
            strategy_entry("S", false, kNaN, (k == 44 ? t - 10.0 : k == 45 ? t - 1.0 : t) / 100.0);
            if (v_.short_reentry_exit || k < 46)
                strategy_exit("SX", "S", (t - 3.0) / 100.0, (t + 10.0) / 100.0);
        }
    }
    // The bracket legs still working: TradingView's filled strategy.exit
    // takes its other leg with it, and a bracket that never filled is the
    // position's own.
    int working_exit_legs() const {
        int legs = 0;
        for (const auto& row : native_working_requests()) {
            const auto& label = row.definition->request.label;
            if (label == "LX" || label == "SX") ++legs;
        }
        return legs;
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

bool near(double a, double b) { return std::fabs(a - b) <= 1e-9; }

void replay(const Variant& v) {
    std::printf("-- %s [%s]\n", v.slug, v.magnifier ? "aggregated 1 -> 15, magnifier"
                                                     : "chart 15 -> 15");
    static const std::vector<Bar> fifteen = feed(kFord15);
    static const std::vector<Bar> one = feed(kFord1m);
    ReentryHost host(v);
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
    // Four rows, the control's last one still open when the data ends.
    CHECK(closed.size() == (v.short_reentry_exit ? 4u : 3u));
    CHECK((open_at_end != nullptr) == !v.short_reentry_exit);
    const auto rows = host.rows();
    CHECK(rows.size() == v.booked);
    int differing = 0;
    for (std::size_t i = 0; i < rows.size() && i < closed.size(); ++i) {
        const TapeTrade& tv = closed[i];
        const ReentryHost::Row& r = rows[i];
        const bool ok = r.is_long == tv.is_long && r.entry_ms == tv.entry_ms
            && r.exit_ms == tv.exit_ms && near(r.entry_price, tv.entry_price)
            && near(r.exit_price, tv.exit_price) && r.qty == tv.qty
            && r.entry_id == tv.entry_signal && r.exit_id == tv.exit_signal
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
                        r.exit_price, r.exit_id.c_str(), r.qty);
        }
    }
    // TradingView's open trade at the end of the data (its exit signal
    // empty) is the engine's open position: same side and quantity. The
    // bracket tapes end flat, the recorded magnified divergence included (its
    // re-entry is never placed); the control ends short 100, the re-entry
    // with no bracket.
    const double tv_open = open_at_end
        ? (open_at_end->is_long ? open_at_end->qty : -open_at_end->qty) : 0.0;
    CHECK(host.open_units() == tv_open);
    // No bracket leg is left working: a filled strategy.exit took its other
    // leg with it however late its receipt was read -- in the control, the
    // stop of the short's bracket, which the drain placed after its limit had
    // filled.
    CHECK(host.working_exit_legs() == 0);
    std::printf("      %zu closed rows (TradingView %zu), %d differ; open at the end %g"
                " (TradingView %g); exit legs working %d\n", rows.size(), closed.size(),
                differing, host.open_units(), tv_open, host.working_exit_legs());
}

}  // namespace

int main() {
    const Variant variants[] = {
        {"int26-reentry-bracket-chart", false, 4, true},
        {"int26-reentry-bracket-mag", true, 3, true},
        {"int26-reentry-noexit-chart", false, 3, false},
    };
    for (const Variant& v : variants) replay(v);
    std::printf("\n%s process_orders_on_close stop re-entry bracket tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
