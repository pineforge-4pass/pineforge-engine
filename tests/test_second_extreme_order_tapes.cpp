/*
 * test_second_extreme_order_tapes.cpp -- R5 lanes PAR-ORDERS-2 (items 2 and 4)
 * and PAR-ORDERS-3 (findings 1, 2 and 4).
 *
 * Under calc_on_order_fills, the recalculation a fill starts may sit exactly
 * on the bar's SECOND extreme -- the end of the leg that approaches it, the
 * high of a low-first bar or the low of a high-first one. H-MEASURE Finding
 * 6d: TradingView fills a market entry that recalculation places at that
 * extreme, where ab9714be (and this engine until lane PAR-ORDERS-2) advanced
 * it to the close, which admits no cascade order, and so to the next open
 * (tests/test_magnified_aggregated_tape.cpp row 5). The same holds for the
 * orders beside it: an exit the recalculation places is live from that point,
 * and a strategy.close fills there.
 *
 * The lane's own synthetic script, NYSE:F 15m over the three days of
 * tests/fixtures/session_islastbar (`lab tv`, ws-report-v1, rangeProof covered;
 * tests/fixtures/second_extreme_orders, whose README names each tape): lot A
 * is open; lot B is a stop entry placed exactly on bar j's second extreme, so
 * B's fill starts the recalculation there with A still open (pyramiding 2),
 * and that recalculation
 *   pa2-i2-exit-w2-*   places strategy.exit("AX", "A", stop, qty=100) with the
 *                      stop between the bar's close and the extreme: reached on
 *                      the extreme -> close leg;
 *   pa2-i4-close-w2-*  calls strategy.close("A").
 * Long lots on low-first bars (cycles j = 7, 19, 26, 42), short lots on
 * high-first ones (j = 5, 30, 46, 51); with and without
 * process_orders_on_close. Everything is closed on bar j+1.
 *
 * TradingView, on every long cycle and the first short one: A's exit fills on
 * bar j, and A's close fills at the extreme's booked tick on bar j. The short
 * tapes are compared on their first cycle only: on bar 30 TradingView never
 * fills B's sell stop at the off-grid 11.795 (the bar's low) and fills it on
 * bar 38 instead, while this engine fills it on bar 30 -- a separate stop-touch
 * question on an off-grid level, which shifts every later short cycle.
 *
 * The boundary: B's fill is the matcher's, at B's own level, which ENDS the
 * leg to the extreme. A fill the calc_on_order_fills path books AT the extreme
 * itself -- an order a recalculation of the bar placed, gap-filled there --
 * STARTS the next leg, and the market orders its recalculation places advance
 * to the close (process_orders_on_close) or the next open, as ab9714be has it
 * (corpus bracket-rivet-calc-on-fill-01 trades 350 and 516). Two more probe
 * bodies, one flat lot at a time (pyramiding 1), on every cycle:
 *   pa2-i2-point-w2-*  P's stop exit PX fills on the leg to the FIRST extreme;
 *                      the recalculation places the market entry A, which
 *                      fills on that extreme; A's recalculation places
 *                      strategy.exit("AX", "A", limit) between the extremes,
 *                      which gap-fills at the second; AX's recalculation
 *                      places the market entry C (long j = 3, 41; short
 *                      j = 2, 30);
 *   pa2-i2-level-w1-*  A is a limit entry AT the first extreme, AX and C as
 *                      above (long j = 4, 25, 40; short j = 2, 32, 44).
 * TradingView: point-w2 books AX at the second extreme and C at the close /
 * next open; level-w1 books AX at its own level on the leg to the second
 * extreme and C at that extreme. Exact since R5 lane PAR-ORDERS-3 (findings 1
 * and 2): exit() reads the recalculating fill -- the matcher's fill at a
 * resting request's own level on the extreme leaves no remainder of its leg in
 * flight, and a fill forced onto the extreme starts the next leg, read from
 * the extreme itself, whose booked tick an off-grid low (11.425 -> 11.43,
 * 11.645 -> 11.65) does not equal. The process_orders_on_close short tape is
 * compared on its first cycle: on bar 31 TradingView fills A's sell limit at
 * the close its high already reached, where the engine fills it on bar 32.
 *
 * R5 lane PAR-ORDERS-3 carries the rule to the FIRST extreme (finding 4):
 * after the matcher's fill at a resting request's own level ON a bar's first
 * extreme, the orders its recalculation places act from that extreme --
 *   pa3-f4-level-w1-{mkt,close,closeall,order}-*  a market entry B,
 *       strategy.close("A"), close_all or a market strategy.order fill AT it;
 *   pa3-f4-level-w1-xstop-*  A's exit stop, already through, fills AT it;
 *   pa3-f4-level-w1-bstop-*  a stop entry B between the extremes fills at its
 *       own level on the leg to the second extreme;
 *   pa3-f4-stop-w1-mkt-*, pa3-f4-xlimit-w1-reentry{,-tp}-*  the two shapes
 *       ab9714be's rows pinned: a STOP entry exactly on the extreme, then the
 *       market add; an EXIT limit exactly on it, then the market re-entry,
 *       whose own exit limit, already through, fills where the path next
 *       re-crosses its level -- ab9714be advanced every such order to the
 *       second extreme or the next open;
 *   pa3-f4-point-w1-mkt-*  the control: a point fill forced onto the first
 *       extreme starts the next leg, and B fills at the second extreme.
 * The same reading moves the exit tapes' rows 5 and 7 (long) and 1 (short): a
 * stop B's recalculation places on the second extreme, reached on the leg to
 * the close, fills at its own level there, as TradingView books it, where it
 * used to be forced onto the close's tick. Every compared row is exact: side,
 * instants, prices, quantity, signals, bars.
 *
 * Fail-before, this TU against R5 lane PAR-ORDERS-2's base (6945fc19): every
 * row of the long exit tapes (8 of 8; 7 of 8 with process_orders_on_close)
 * exits A at the next open, and every close tape row closes A at the next open
 * (or, under process_orders_on_close, at the bar's close, then B by the
 * re-issued close). Against R5 lane PAR-ORDERS-3's base (9f7a025c): 79 of 285
 * checks fail -- every level-w1 row, point-w2 long's rows 2, 3, 5, 6 (2, 3, 5
 * under process_orders_on_close), the exit tapes' rows 5, 7 / 1, and every
 * pa3-f4 order but the point-w1 controls'.
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

#ifndef PINEFORGE_PA2_SECOND_EXTREME_FIXTURE_DIR
#error "PINEFORGE_PA2_SECOND_EXTREME_FIXTURE_DIR must name tests/fixtures/second_extreme_orders"
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
    std::ifstream in(std::string(PINEFORGE_PA2_SECOND_EXTREME_FIXTURE_DIR) + "/" + slug
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

// Which probe body a tape ran: lot B's stop on the second extreme (items 2
// and 4), or the boundary pair -- A's limit on the FIRST extreme (a level
// fill) or A's market fill ON it (a point fill), each followed by the exit AX
// and the market entry C.
enum class Probe {
    SecondExtremeStop, LevelFirstExtreme, PointSecondExtreme,
    // R5 lane PAR-ORDERS-3, finding 4: the order the recalculation on A's
    // fill places -- a market entry B, strategy.close("A"), close_all, a
    // market strategy.order B, A's exit AX with its stop already through, or
    // a stop entry B between the extremes -- A's fill a level fill AT the
    // first extreme, or (the control) a point fill forced onto it.
    LevelFirstMarket, LevelFirstClose, LevelFirstCloseAll, LevelFirstOrder,
    LevelFirstExitStop, LevelFirstStopEntry, PointFirstMarket,
    // ... and the two shapes ab9714be's rows pin: A a STOP entry exactly on the
    // first extreme, then the market add B; E's EXIT limit X exactly on it,
    // then the market re-entry E -- and that re-entry's own exit limit X2
    // between the extremes, already marketable at its price.
    StopFirstMarket, ExitLimitFirstReentry, ExitLimitFirstReentryTp,
};
bool first_extreme_probe(Probe p) {
    return p == Probe::LevelFirstMarket || p == Probe::LevelFirstClose
        || p == Probe::LevelFirstCloseAll || p == Probe::LevelFirstOrder
        || p == Probe::LevelFirstExitStop || p == Probe::LevelFirstStopEntry
        || p == Probe::PointFirstMarket || p == Probe::StopFirstMarket
        || p == Probe::ExitLimitFirstReentry || p == Probe::ExitLimitFirstReentryTp;
}

struct Variant {
    const char* slug;
    bool is_long;
    bool exit_leg;   // strategy.exit (items 2) or strategy.close (item 4)
    bool pooc;
    std::size_t compared;  // rows compared (the short tapes: their first cycle)
    unsigned recorded;     // bit i+1: row i+1 is a RECORDED divergence
    Probe probe = Probe::SecondExtremeStop;
};

// The Pine probe's body (tests/fixtures/second_extreme_orders/<slug>/strategy.pine).
class ExtremeHost final : public source::PineStrategyHost {
public:
    explicit ExtremeHost(const Variant& v) : v_(v) {
        set_syminfo_session("0930-1600");
        set_syminfo_timezone("America/New_York");
        source::PineStrategyConfig c;
        c.initial_capital = 100000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 100;
        c.pyramiding = v.probe == Probe::ExitLimitFirstReentry
                || v.probe == Probe::ExitLimitFirstReentryTp ? 1
            : v.probe == Probe::SecondExtremeStop || first_extreme_probe(v.probe) ? 2 : 1;
        c.process_orders_on_close = v.pooc;
        c.calc_on_order_fills = true;
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
        if (first_extreme_probe(v_.probe)) {
            first_extreme(k, units);
            return;
        }
        if (v_.probe != Probe::SecondExtremeStop) {
            boundary(k, units);
            return;
        }
        static const int kLong[] = {7, 19, 26, 42};
        static const double kLongStop[] = {11.665, 11.715, 11.96, 11.785};
        static const double kLongAx[] = {11.66, 11.71, 11.95, 11.77};
        static const int kShort[] = {5, 30, 46, 51};
        static const double kShortStop[] = {11.44, 11.795, 11.61, 11.49};
        static const double kShortAx[] = {11.47, 11.83, 11.615, 11.52};
        const int* js = v_.is_long ? kLong : kShort;
        const double* stops = v_.is_long ? kLongStop : kShortStop;
        const double* ax = v_.is_long ? kLongAx : kShortAx;
        for (int c = 0; c < 4; ++c) {
            const int j = js[c];
            if (k == j - 2 && units == 0.0) strategy_entry("A", v_.is_long);
            if (k == j - 1) strategy_entry("B", v_.is_long, kNaN, stops[c]);
            if (k == j + 1 && units != 0.0) strategy_close_all();
        }
        // B's fill opened the second lot: this is the recalculation on the
        // second extreme (or, when A has not gone yet, the bar's close).
        if (std::fabs(units) > 150.0) {
            if (!v_.exit_leg) {
                strategy_close("A");
                return;
            }
            for (int c = 0; c < 4; ++c) {
                if (k == js[c])
                    strategy_exit("AX", "A", kNaN, ax[c], kNaN, kNaN, kNaN, 100.0, {}, 100.0);
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
    // strategy.closedtrades - strategy.closedtrades[1]: the history slot is
    // the count the bar's last calculation saw.
    int closed_now(int k) {
        if (k != bar_) {
            closed_before_ = closed_seen_;
            bar_ = k;
        }
        closed_seen_ = trade_count();
        return closed_seen_ - closed_before_;
    }

    void boundary(int k, double units) {
        const bool level = v_.probe == Probe::LevelFirstExtreme;
        static const int kLevelLong[] = {4, 25, 40};
        static const double kLevelLongA[] = {11.46, 11.73, 11.61};
        static const double kLevelLongAx[] = {11.50, 11.76, 11.70};
        static const int kLevelShort[] = {2, 32, 44};
        static const double kLevelShortA[] = {11.48, 11.86, 11.72};
        static const double kLevelShortAx[] = {11.43, 11.82, 11.69};
        static const int kPointLong[] = {3, 41};
        static const double kPointLongPx[] = {11.44, 11.66};
        static const double kPointLongAx[] = {11.47, 11.70};
        static const int kPointShort[] = {2, 30};
        static const double kPointShortPx[] = {11.46, 11.85};
        static const double kPointShortAx[] = {11.43, 11.82};
        const int cycles = level ? 3 : 2;
        const int* js = level ? (v_.is_long ? kLevelLong : kLevelShort)
                              : (v_.is_long ? kPointLong : kPointShort);
        const double* placed = level ? (v_.is_long ? kLevelLongA : kLevelShortA)
                                     : (v_.is_long ? kPointLongPx : kPointShortPx);
        const double* ax = level ? (v_.is_long ? kLevelLongAx : kLevelShortAx)
                                 : (v_.is_long ? kPointLongAx : kPointShortAx);
        const int closed = closed_now(k);
        const bool live = v_.is_long ? units > 0.0 : units < 0.0;
        for (int c = 0; c < cycles; ++c) {
            const int j = js[c];
            if (k == j - 1 && units == 0.0) {
                if (level) {
                    strategy_entry("A", v_.is_long, placed[c]);
                } else {
                    strategy_entry("P", v_.is_long);
                    strategy_exit("PX", "P", kNaN, placed[c]);
                }
            }
            if (k == j) {
                if (level) {
                    if (live) strategy_exit("AX", "A", ax[c], kNaN);
                    if (units == 0.0 && closed == 1) strategy_entry("C", v_.is_long);
                } else {
                    if (units == 0.0 && closed == 1) strategy_entry("A", v_.is_long);
                    if (live && open_trade_entry_id(0) == "A")
                        strategy_exit("AX", "A", ax[c], kNaN);
                    if (units == 0.0 && closed == 2) strategy_entry("C", v_.is_long);
                }
            }
            if (k == j + 2 && units != 0.0) strategy_close_all();
        }
    }

    void first_extreme(int k, double units) {
        if (v_.probe == Probe::StopFirstMarket || v_.probe == Probe::ExitLimitFirstReentry
            || v_.probe == Probe::ExitLimitFirstReentryTp) {
            first_extreme_pinned_shapes(k, units);
            return;
        }
        const bool point = v_.probe == Probe::PointFirstMarket;
        static const int kLevelLong[] = {4, 25, 40};
        static const double kLevelLongA[] = {11.46, 11.73, 11.61};
        static const int kLevelShort[] = {2, 44};
        static const double kLevelShortA[] = {11.48, 11.72};
        static const int kPointLong[] = {3, 41};
        static const double kPointLongPx[] = {11.44, 11.66};
        static const int kPointShort[] = {2, 30};
        static const double kPointShortPx[] = {11.46, 11.85};
        static const double kLongXStop[] = {11.48, 11.75, 11.63};
        static const double kShortXStop[] = {11.46, 11.70};
        static const double kLongBStop[] = {11.50, 11.76, 11.70};
        static const double kShortBStop[] = {11.43, 11.69};
        const int cycles = point || !v_.is_long ? 2 : 3;
        const int* js = point ? (v_.is_long ? kPointLong : kPointShort)
                              : (v_.is_long ? kLevelLong : kLevelShort);
        const double* placed = point ? (v_.is_long ? kPointLongPx : kPointShortPx)
                                     : (v_.is_long ? kLevelLongA : kLevelShortA);
        const int closed = closed_now(k);
        const double held = v_.is_long ? 100.0 : -100.0;
        for (int c = 0; c < cycles; ++c) {
            const int j = js[c];
            if (k == j - 1 && units == 0.0) {
                if (point) {
                    strategy_entry("P", v_.is_long);
                    strategy_exit("PX", "P", kNaN, placed[c]);
                } else {
                    strategy_entry("A", v_.is_long, placed[c]);
                }
            }
            if (k == j) {
                if (point && units == 0.0 && closed == 1) strategy_entry("A", v_.is_long);
                if (units == held && (!point || open_trade_entry_id(0) == "A")) {
                    switch (v_.probe) {
                    case Probe::LevelFirstClose: strategy_close("A"); break;
                    // codegen spells strategy.close_all() as a close of "".
                    case Probe::LevelFirstCloseAll: strategy_close(""); break;
                    case Probe::LevelFirstOrder: strategy_order("B", v_.is_long, 100.0); break;
                    case Probe::LevelFirstExitStop:
                        strategy_exit("AX", "A", kNaN,
                                      (v_.is_long ? kLongXStop : kShortXStop)[c]);
                        break;
                    case Probe::LevelFirstStopEntry:
                        strategy_entry("B", v_.is_long, kNaN,
                                       (v_.is_long ? kLongBStop : kShortBStop)[c]);
                        break;
                    default: strategy_entry("B", v_.is_long); break;
                    }
                }
            }
            // The later scripts cancel what is still working before the close.
            const bool cancels = v_.probe == Probe::LevelFirstCloseAll
                || v_.probe == Probe::LevelFirstOrder || v_.probe == Probe::LevelFirstExitStop
                || v_.probe == Probe::LevelFirstStopEntry;
            if (k == j + 2 && units != 0.0) {
                if (cancels) strategy_cancel_all();
                strategy_close_all();
            }
        }
    }

    void first_extreme_pinned_shapes(int k, double units) {
        // A buy stop / exit limit at the high of a high-first bar (long), a sell
        // stop / exit limit at the low of a low-first one (short).
        static const int kLong[] = {2, 32, 44};
        static const double kLongLevel[] = {11.48, 11.86, 11.72};
        static const int kShort[] = {4, 25, 40};
        static const double kShortLevel[] = {11.46, 11.73, 11.61};
        static const double kLongTp[] = {11.44, 11.83, 11.69};
        static const double kShortTp[] = {11.49, 11.76, 11.67};
        const int* js = v_.is_long ? kLong : kShort;
        const double* level = v_.is_long ? kLongLevel : kShortLevel;
        const double* tp = v_.is_long ? kLongTp : kShortTp;
        const int closed = closed_now(k);
        const double held = v_.is_long ? 100.0 : -100.0;
        const bool stop_entry = v_.probe == Probe::StopFirstMarket;
        for (int c = 0; c < 3; ++c) {
            const int j = js[c];
            if (stop_entry) {
                if (k == j - 1 && units == 0.0) strategy_entry("A", v_.is_long, kNaN, level[c]);
                if (k == j && units == held) strategy_entry("B", v_.is_long);
            } else {
                if (k == j - 1 && units == 0.0) strategy_entry("E", v_.is_long);
                if (k == j && units == held && closed == 0)
                    strategy_exit("X", "E", level[c], kNaN);
                if (k == j && units == 0.0 && closed == 1) strategy_entry("E", v_.is_long);
                if (v_.probe == Probe::ExitLimitFirstReentryTp && k == j && units == held
                    && closed == 1)
                    strategy_exit("X2", "E", tp[c], kNaN);
            }
            if (k == j + 2 && units != 0.0) {
                strategy_cancel_all();
                strategy_close("");
            }
        }
    }

    Variant v_;
    int bar_ = -1;
    int closed_before_ = 0;
    int closed_seen_ = 0;
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
std::string exit_signal(const ExtremeHost::Row& r) {
    if (r.exit_id.rfind("__close__", 0) == 0 && r.exit_id.size() > 9)
        return "Close entry(s) order " + r.exit_id.substr(9);
    return r.exit_id.rfind("__", 0) == 0 ? "Close position order" : r.exit_id;
}

bool near(double a, double b) { return std::fabs(a - b) <= 1e-9; }

void replay(const Variant& v) {
    std::printf("-- %s\n", v.slug);
    static const std::vector<Bar> fifteen = feed(kFord15);
    ExtremeHost host(v);
    host.run(fifteen.data(), static_cast<int>(fifteen.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    const auto tape = read_tape(v.slug);
    const auto rows = host.rows();
    CHECK(tape.size() >= v.compared);
    CHECK(rows.size() >= v.compared);
    int differing = 0;
    for (std::size_t i = 0; i < v.compared && i < rows.size() && i < tape.size(); ++i) {
        const TapeTrade& tv = tape[i];
        const ExtremeHost::Row& r = rows[i];
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
    std::printf("      %zu rows compared, %d differ\n", v.compared, differing);
}

}  // namespace

int main() {
    const Variant variants[] = {
        {"pa2-i2-exit-w2-long", true, true, false, 8, 0u},
        {"pa2-i2-exit-w2-long-pooc", true, true, true, 8, 0u},
        {"pa2-i2-exit-w2-short", false, true, false, 2, 0u},
        {"pa2-i2-exit-w2-short-pooc", false, true, true, 2, 0u},
        {"pa2-i4-close-w2-long", true, false, false, 8, 0u},
        {"pa2-i4-close-w2-long-pooc", true, false, true, 8, 0u},
        {"pa2-i4-close-w2-short", false, false, false, 2, 0u},
        {"pa2-i4-close-w2-short-pooc", false, false, true, 2, 0u},
        {"pa2-i2-level-w1-long", true, true, false, 6, 0u, Probe::LevelFirstExtreme},
        {"pa2-i2-level-w1-long-pooc", true, true, true, 6, 0u, Probe::LevelFirstExtreme},
        {"pa2-i2-level-w1-short", false, true, false, 6, 0u, Probe::LevelFirstExtreme},
        {"pa2-i2-level-w1-short-pooc", false, true, true, 2, 0u, Probe::LevelFirstExtreme},
        {"pa2-i2-point-w2-long", true, true, false, 6, 0u, Probe::PointSecondExtreme},
        {"pa2-i2-point-w2-long-pooc", true, true, true, 6, 0u, Probe::PointSecondExtreme},
        {"pa2-i2-point-w2-short", false, true, false, 6, 0u, Probe::PointSecondExtreme},
        {"pa2-i2-point-w2-short-pooc", false, true, true, 6, 0u, Probe::PointSecondExtreme},
        {"pa3-f4-level-w1-mkt-long", true, true, false, 6, 0u, Probe::LevelFirstMarket},
        {"pa3-f4-level-w1-mkt-short", false, true, false, 4, 0u, Probe::LevelFirstMarket},
        {"pa3-f4-level-w1-close-long", true, true, false, 3, 0u, Probe::LevelFirstClose},
        {"pa3-f4-level-w1-close-short", false, true, false, 2, 0u, Probe::LevelFirstClose},
        {"pa3-f4-level-w1-closeall-long", true, true, false, 3, 0u, Probe::LevelFirstCloseAll},
        {"pa3-f4-level-w1-closeall-short", false, true, false, 2, 0u, Probe::LevelFirstCloseAll},
        {"pa3-f4-level-w1-order-long", true, true, false, 6, 0u, Probe::LevelFirstOrder},
        {"pa3-f4-level-w1-order-short", false, true, false, 4, 0u, Probe::LevelFirstOrder},
        {"pa3-f4-level-w1-xstop-long", true, true, false, 3, 0u, Probe::LevelFirstExitStop},
        {"pa3-f4-level-w1-xstop-short", false, true, false, 2, 0u, Probe::LevelFirstExitStop},
        {"pa3-f4-level-w1-bstop-long", true, true, false, 6, 0u, Probe::LevelFirstStopEntry},
        {"pa3-f4-level-w1-bstop-short", false, true, false, 4, 0u, Probe::LevelFirstStopEntry},
        {"pa3-f4-stop-w1-mkt-long", true, true, false, 6, 0u, Probe::StopFirstMarket},
        {"pa3-f4-stop-w1-mkt-short", false, true, false, 6, 0u, Probe::StopFirstMarket},
        {"pa3-f4-xlimit-w1-reentry-long", true, true, false, 6, 0u, Probe::ExitLimitFirstReentry},
        {"pa3-f4-xlimit-w1-reentry-short", false, true, false, 6, 0u, Probe::ExitLimitFirstReentry},
        {"pa3-f4-xlimit-w1-reentry-tp-long", true, true, false, 6, 0u,
         Probe::ExitLimitFirstReentryTp},
        {"pa3-f4-xlimit-w1-reentry-tp-short", false, true, false, 6, 0u,
         Probe::ExitLimitFirstReentryTp},
        {"pa3-f4-point-w1-mkt-long", true, true, false, 6, 0u, Probe::PointFirstMarket},
        {"pa3-f4-point-w1-mkt-short", false, true, false, 6, 0u, Probe::PointFirstMarket},
    };
    for (const Variant& v : variants) replay(v);
    std::printf("\n%s second-extreme order tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
