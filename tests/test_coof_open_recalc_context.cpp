/*
 * test_coof_open_recalc_context.cpp — what a calc_on_order_fills FILL RECALC
 * sees, and how a default-sized market order it places is sized (round 7
 * family M, officialjackofalltrades aureate BTC@1D; campaign note
 * log-20260905t121513z-50167cb8, which CORRECTS the m1d-coof-ctx pin).
 *
 * Rule 1 — script context (unchanged engine behaviour, pinned here as a
 * regression guard): TradingView executes the on-fill recalculation on the
 * CURRENT bar's FINAL values — high/low/close/volume of bar k, bar_index k,
 * dayofmonth of bar k, close[1] = bar k-1, barstate.isconfirmed TRUE — for a
 * fill at bar k's OPEN and for an intrabar fill alike. Never bar k-1's series,
 * never the open tick. Evidence: lab tv sensor tapes scratchpad/pins/
 * m1d-coof-ctx2-btc (BINANCE:BTCUSDT 1D 2025-10-01..12-31, tv_trades sha256
 * f2e8418e…) and m1d-coof-ctx2-f (NYSE:F 1D, gaps): four encoded orders fire
 * in the first calc with position_size == 1 — inside the recalc after a market
 * fill at O, or after an intrabar limit fill at close*0.995 — and 103/103
 * firings decode as finals/k/k/prev/confirmed. The earlier m1d-coof-ctx sensor
 * placed its order at an ordinary close calc (its L close fills in the recalc at
 * the same open, so no fill ever happens at the ≡2 bar's open) and therefore
 * never tested the context at all.
 *
 * Rule 2 — sizing (the engine change): a DEFAULT-sized percent_of_equity / cash
 * MARKET order born in a fill recalc is sized by TradingView at ITS OWN FILL,
 * not at the signal bar's close (the ordinary freeze) and not at the recalc's
 * cursor. Evidence: scratchpad/pins/m1d-coof-size-btc (tv_trades sha256
 * 7ee8712b…): "B", born in the second recalc at the 10-02 open and filled at
 * W1 = the low 118279.31, has qty 845.4564 = 10% x 1e9 / 118279.31 (cursor O
 * 118594.99 -> 843.2, the bar's close 120529.35 -> 829.7); thirteen entries
 * born in a first-O recalc and filled at O size at O. The probe: TV 4 0.09245
 * = 9802.56 / (106011.13 x 1.0001) at the 11-11 open fill (the engine froze
 * 0.0951 at the 11-11 close 103058.99); TV 10 0.14674 at its W2 fill 69988.83
 * (cursor W1 63913.27 -> 0.16069, close 67988.04 -> 0.15106).
 *
 * Both tapes are replayed on the registry bars (BINANCE:BTCUSDT 1D, feed
 * 14b8e066225c; test_coof_open_recalc_context_data.hpp). Context: (a) the tape
 * itself decodes to the registry bar's finals at all 45 firings, (b) the engine
 * re-creates each firing's triggering fill (a carried 1-lot market order at O,
 * or the 1-lot limit at close*0.995 — carried, or born in the open recalc when
 * TradingView's fill price says so) and its recalc context encodes to the
 * same four numbers, in a fill recalc, confirmed, with the trigger filled at
 * TradingView's price. Sizing: (a) the tape's recalc-born rows fit the fill
 * and its ordinary rows fit the signal close (both discriminated by BTC's
 * one-cent open/close offsets), (b) the engine reproduces the first cycle
 * (A at O, B sized at W1) and the JOAT rows TV 3/4 and TV 8/9/10 (entry and
 * exit bar/price, quantity, net PnL). Controls: COOF off is byte-identical
 * (the ordinary signal-close freeze), a close-calc placement inside a COOF
 * run still freezes, and an intrabar fill recalc sees the finals and sizes
 * its cascade entry at the W1 fill. On the pre-change engine the sizing
 * assertions fail (0.0951 / 0.15103 / 0.15106 / 829.67 / 817.05) and every
 * context assertion passes.
 *
 * Known, deliberately NOT asserted (cascade machinery, out of this change's
 * scope, recorded in the campaign notes): TradingView fills EVERY market order
 * born in the first-O recalc at O (S1..S4 and the close_all on 10-02), fills
 * every sibling born in one mid-bar recalc at the same waypoint (S1..S4 at W1
 * on 10-04), and rolls the close_alls born in the recalcs after those W1
 * fills to the next open; the engine's KI-60/67 scheduler admits one more
 * fill at O and one fill per waypoint. That is why the sensor tapes are not
 * replayed row-for-row here.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

#include "test_coof_open_recalc_context_data.hpp"

using namespace pineforge;
using namespace coof_context_data;

static int tests_passed = 0;
static int tests_failed = 0;
static bool g_dump = false;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        double _a = (a), _b = (b);                                             \
        if (!(std::fabs(_a - _b) <= (tol))) {                                  \
            std::printf("  FAIL  %s:%d  %s == %.10f, expected %.10f\n",        \
                        __FILE__, __LINE__, #a, _a, _b);                       \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int64_t kDayMs = 86'400'000;

int64_t day_ts(int y, int m, int d) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    return static_cast<int64_t>(timegm(&t)) * 1000;
}

int day_of_month(int64_t ts_ms) {
    std::time_t s = static_cast<std::time_t>(ts_ms / 1000);
    std::tm t{};
    gmtime_r(&s, &t);
    return t.tm_mday;
}

std::string iso_day(int64_t ts_ms) {
    std::time_t s = static_cast<std::time_t>(ts_ms / 1000);
    std::tm t{};
    gmtime_r(&s, &t);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", t.tm_year + 1900,
                  t.tm_mon + 1, t.tm_mday);
    return buf;
}

int index_of_day(const std::vector<Bar>& bars, int64_t ts) {
    for (size_t i = 0; i < bars.size(); ++i) {
        if (bars[i].timestamp == ts) return static_cast<int>(i);
    }
    return -1;
}

// BINANCE:BTCUSDT: 0.01 tick, 1e-5 lot step (the tapes carry 5-decimal
// quantities).
class BtcProbe : public BacktestEngine {
public:
    explicit BtcProbe(bool coof) {
        calc_on_order_fills_ = coof;
        syminfo_.pointvalue = 1.0;
        syminfo_mintick_ = 0.01;
        qty_step_ = 0.00001;
        slippage_ = 0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
    }
    bool flat() const { return position_side_ == PositionSide::FLAT; }
    bool is_long_pos() const { return position_side_ == PositionSide::LONG; }
    bool is_short_pos() const { return position_side_ == PositionSide::SHORT; }
    double pos_qty() const { return flat() ? 0.0 : position_qty_; }
    int opentrades() const { return static_cast<int>(pyramid_entries_.size()); }
    std::string entry_id(size_t i) const {
        return i < pyramid_entries_.size() ? pyramid_entries_[i].entry_id : "";
    }
    bool confirmed() const { return is_last_tick_; }
    const Bar& bar() const { return current_bar_; }
    int bar_idx() const { return bar_index_; }
    int trades() const { return static_cast<int>(trades_.size()); }
    bool recalc_active() const { return coof_fill_recalc_active_; }
};

void dump_trades(const BacktestEngine& e, const char* title) {
    if (!g_dump) return;
    std::printf("--- %s: %d trades\n", title, e.trade_count());
    for (int i = 0; i < e.trade_count(); ++i) {
        const Trade& t = e.get_trade(i);
        std::printf("  %3d %-6s %s %s @%.5f qty %.5f -> %s @%.5f pnl %.5f %s\n",
                    i + 1, t.entry_id.c_str(), t.is_long ? "L" : "S",
                    iso_day(t.entry_time).c_str(), t.entry_price, t.qty,
                    iso_day(t.exit_time).c_str(), t.exit_price, t.pnl,
                    t.exit_comment.c_str());
    }
}

// ---------------------------------------------------------------------------
// Rule 1, the tape itself: every TV firing of m1d-coof-ctx2 decodes to the
// registry bar's FINALS (h-l, c-o, volume), bar_index k (10-01 is ≡0 mod 4 on
// TradingView's chart, one bar per day), dayofmonth k, close[1] = bar k-1 and
// barstate.isconfirmed = 1. The encodings, from the sensor's Pine source:
//     S1 = isconfirmed*1e7 + round((h-l)/10)*1e3 + 500 + round((c-o)/10)
//     S2 = (bar_index%4)*1e6 + dayofmonth*1e3 + min(999, round(v/1000))
//     S3 = 1e7 + round((h[1]-l[1])/10)*1e3 + 500 + round((c[1]-o[1])/10)
//     S4 = 1e7 + round(v) % 1e7
// ---------------------------------------------------------------------------
struct Encoded { double s1, s2, s3, s4; };

Encoded encode_context(const Bar& b, const Bar& prev, bool confirmed, int cyc) {
    Encoded e;
    e.s1 = (confirmed ? 1e7 : 0.0)
        + std::llround((b.high - b.low) / 10.0) * 1000.0 + 500.0
        + std::llround((b.close - b.open) / 10.0);
    e.s2 = cyc * 1e6 + day_of_month(b.timestamp) * 1000.0
        + std::min<long long>(999, std::llround(b.volume / 1000.0));
    e.s3 = 1e7 + std::llround((prev.high - prev.low) / 10.0) * 1000.0 + 500.0
        + std::llround((prev.close - prev.open) / 10.0);
    e.s4 = 1e7 + static_cast<double>(std::llround(b.volume) % 10'000'000LL);
    return e;
}

int tv_cycle(int64_t ts) {   // TradingView's bar_index % 4 on the tape
    return static_cast<int>(((ts - day_ts(2025, 10, 1)) / kDayMs) % 4);
}

struct TvFiring {
    int64_t ts;
    std::string trigger;      // "LM" or "LL"
    double trigger_price;
    Encoded sensors;
};

std::vector<TvFiring> tv_firings() {
    std::map<int64_t, TvFiring> by_ts;
    for (const TapeRow& r : ctx2_tape()) {
        TvFiring& f = by_ts[r.entry_ts];
        f.ts = r.entry_ts;
        if (std::strcmp(r.id, "LM") == 0 || std::strcmp(r.id, "LL") == 0) {
            f.trigger = r.id;
            f.trigger_price = r.entry_price;
        } else if (std::strcmp(r.id, "S1") == 0) f.sensors.s1 = r.qty;
        else if (std::strcmp(r.id, "S2") == 0) f.sensors.s2 = r.qty;
        else if (std::strcmp(r.id, "S3") == 0) f.sensors.s3 = r.qty;
        else if (std::strcmp(r.id, "S4") == 0) f.sensors.s4 = r.qty;
    }
    std::vector<TvFiring> out;
    for (auto& kv : by_ts) {
        if (kv.second.sensors.s1 > 0.0) out.push_back(kv.second);
    }
    return out;
}

void test_context_tape_decodes_to_bar_finals() {
    std::printf("test_context_tape_decodes_to_bar_finals\n");
    const auto bars = btc_1d_autumn_bars();
    const auto firings = tv_firings();
    CHECK(firings.size() == 45);
    int open_fills = 0;
    int intrabar_fills = 0;
    for (const TvFiring& f : firings) {
        const int k = index_of_day(bars, f.ts);
        CHECK(k > 0);
        if (k <= 0) continue;
        CHECK(!f.trigger.empty());
        const Encoded e = encode_context(bars[k], bars[k - 1], true, tv_cycle(f.ts));
        const bool ok = e.s1 == f.sensors.s1 && e.s2 == f.sensors.s2
            && e.s3 == f.sensors.s3 && e.s4 == f.sensors.s4;
        if (!ok) {
            std::printf("  FAIL  %s: finals encode %.0f %.0f %.0f %.0f, TV %.0f %.0f %.0f %.0f\n",
                        iso_day(f.ts).c_str(), e.s1, e.s2, e.s3, e.s4,
                        f.sensors.s1, f.sensors.s2, f.sensors.s3, f.sensors.s4);
        }
        CHECK(ok);
        // And NOT the previous bar's values, nor the open tick (0/0):
        const Encoded prev_e = encode_context(bars[k - 1], bars[k - 2], true, tv_cycle(f.ts));
        CHECK(prev_e.s1 != f.sensors.s1);
        CHECK(f.sensors.s1 != 1e7 + 500.0);
        if (std::fabs(f.trigger_price - bars[k].open) < 1e-6) ++open_fills;
        else ++intrabar_fills;
    }
    // Both kinds of triggering fill are represented.
    CHECK(open_fills >= 25);
    CHECK(intrabar_fills >= 15);
}

// ---------------------------------------------------------------------------
// Rule 1, the engine: the same 45 firings replayed on the registry bars. Each
// TV firing on bar D is re-created by its trigger — a 1-lot market order
// carried into D's open, or a 1-lot limit at close*0.995 (placed at D-1's
// close when TradingView's fill price says so, else born in a recalc at D's
// open — which needs a carried 1-lot "X" to fill at that open first). The
// sensor reads the recalc's context at the first calc that holds the trigger
// lot; the ordinary close calc of D flattens everything. No sibling sensor
// orders are placed (see the header: their same-waypoint fills are a cascade
// finding, not this change).
// ---------------------------------------------------------------------------
struct EngineFiring {
    int64_t ts;
    bool in_recalc;
    bool confirmed;
    Encoded sensors;
    double trigger_fill;
};

class ContextReplayProbe final : public BtcProbe {
public:
    struct Plan {
        std::string trigger;
        bool recalc_born;   // LL born in the recalc at D's open (limit from D's close)
        double level;       // LL limit level
    };

    ContextReplayProbe(std::vector<Bar> feed, std::map<int64_t, Plan> plan)
        : BtcProbe(true), feed_(std::move(feed)), plan_(std::move(plan)) {
        initial_capital_ = 1e14;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 10;
        margin_long_ = 0.0;
        margin_short_ = 0.0;
    }
    std::vector<EngineFiring> firings;

    void on_bar(const Bar& b) override {
        const int64_t next_ts = b.timestamp + kDayMs;
        const bool ordinary = !recalc_active();
        // D-1's ordinary close calc: arm tomorrow's trigger.
        auto nx = plan_.find(next_ts);
        if (nx != plan_.end() && ordinary) {
            const Plan& np = nx->second;
            if (np.trigger == "LM") {
                strategy_order("LM", true, 1.0);
            } else if (!np.recalc_born) {
                strategy_order("LL", true, 1.0, np.level);
            } else {
                strategy_order("X", true, 1.0);   // the open fill that opens D's recalc
            }
        }
        auto it = plan_.find(b.timestamp);
        if (it != plan_.end()) {
            const Plan& pl = it->second;
            if (pl.trigger == "LL" && pl.recalc_born && recalc_active()
                && has_lot("X") && !has_lot("LL") && !ll_armed_) {
                ll_armed_ = true;
                strategy_order("LL", true, 1.0, pl.level);
            }
            if (has_lot(pl.trigger) && fired_ != bar_index_) {
                fired_ = bar_index_;
                const Bar& prev = feed_[static_cast<size_t>(bar_index_ - 1)];
                EngineFiring f;
                f.ts = b.timestamp;
                f.in_recalc = recalc_active();
                f.confirmed = confirmed();
                f.sensors = encode_context(b, prev, confirmed(), tv_cycle(b.timestamp));
                f.trigger_fill = lot_price(pl.trigger);
                firings.push_back(f);
            }
            if (ordinary) {
                strategy_cancel("LL");
                strategy_close_all();
                ll_armed_ = false;
            }
        }
    }

private:
    bool has_lot(const std::string& id) const {
        for (const auto& lot : pyramid_entries_) if (lot.entry_id == id) return true;
        return false;
    }
    double lot_price(const std::string& id) const {
        for (const auto& lot : pyramid_entries_) if (lot.entry_id == id) return lot.price;
        return kNaN;
    }
    std::vector<Bar> feed_;
    std::map<int64_t, Plan> plan_;
    int fired_ = -1;
    bool ll_armed_ = false;
};

void test_engine_recalc_context_matches_tape() {
    std::printf("test_engine_recalc_context_matches_tape\n");
    const auto bars = btc_1d_autumn_bars();
    const auto firings = tv_firings();
    std::map<int64_t, ContextReplayProbe::Plan> plan;
    int recalc_born_ll = 0;
    for (const TvFiring& f : firings) {
        const int k = index_of_day(bars, f.ts);
        ContextReplayProbe::Plan pl;
        pl.trigger = f.trigger;
        pl.recalc_born = false;
        pl.level = kNaN;
        if (f.trigger == "LL") {
            // The Pine passes the RAW close*0.995; the engine snaps a buy limit
            // to the tick (floor) exactly as TradingView booked these fills.
            const double prev_level = bars[k - 1].close * 0.995;
            const double cur_level = bars[k].close * 0.995;
            const bool at_open = std::fabs(f.trigger_price - bars[k].open) < 1e-6;
            const bool prev_fits = at_open ? prev_level >= bars[k].open - 1e-6
                                           : std::fabs(prev_level - f.trigger_price) < 0.011;
            if (prev_fits) {
                pl.level = prev_level;
            } else {
                pl.recalc_born = true;
                pl.level = cur_level;
                ++recalc_born_ll;
                const bool cur_fits = at_open ? cur_level >= bars[k].open - 1e-6
                                              : std::fabs(cur_level - f.trigger_price) < 0.011;
                CHECK(cur_fits);
            }
        }
        plan[f.ts] = pl;
    }
    CHECK(recalc_born_ll > 0);
    ContextReplayProbe p(bars, plan);
    p.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(p.last_error().empty());
    dump_trades(p, "context replay");

    CHECK(p.firings.size() == firings.size());
    std::map<int64_t, EngineFiring> engine;
    for (const EngineFiring& f : p.firings) engine[f.ts] = f;
    int matched = 0;
    for (const TvFiring& f : firings) {
        auto it = engine.find(f.ts);
        if (it == engine.end()) {
            std::printf("  FAIL  no engine firing on %s\n", iso_day(f.ts).c_str());
            ++tests_failed;
            continue;
        }
        const EngineFiring& e = it->second;
        const bool ok = e.in_recalc && e.confirmed
            && e.sensors.s1 == f.sensors.s1 && e.sensors.s2 == f.sensors.s2
            && e.sensors.s3 == f.sensors.s3 && e.sensors.s4 == f.sensors.s4;
        if (!ok) {
            std::printf("  FAIL  %s recalc=%d conf=%d engine %.0f %.0f %.0f %.0f, TV %.0f %.0f %.0f %.0f\n",
                        iso_day(f.ts).c_str(), e.in_recalc, e.confirmed,
                        e.sensors.s1, e.sensors.s2, e.sensors.s3, e.sensors.s4,
                        f.sensors.s1, f.sensors.s2, f.sensors.s3, f.sensors.s4);
        }
        CHECK(ok);
        CHECK_NEAR(e.trigger_fill, f.trigger_price, 1e-6);
        if (ok) ++matched;
    }
    CHECK(matched == 45);
}

// ---------------------------------------------------------------------------
// Rule 2, the tape itself: m1d-coof-size discriminates the two placement
// kinds on one chart. Every recalc-born default-sized row — B (born in the
// second recalc at the 10-02 open, filled at W1) and every A after the first
// (born in the first-O recalc after the safety close_all's fill, filled at O)
// — is sized at ITS FILL: qty = floor5(10% x E / fill), E = 1e9 + the
// cumulative PnL before the row (flat at every sizing moment). Every D (an
// ordinary placement at the ≡3 close calc, filled at the next open) and the
// first A (placed at the 10-01 close) are sized at the SIGNAL CLOSE, the
// pinned ordinary rule — on BTC the next open differs from that close by a
// cent often enough to tell the two apart (six A rows, twelve D rows).
// ---------------------------------------------------------------------------
double floor5(double q) { return std::floor(q * 1e5 + 1e-6) / 1e5; }
double tick2(double p) { return std::floor(p / 0.01 + 0.5) * 0.01; }

void test_sizing_tape_sizes_recalc_born_at_fill() {
    std::printf("test_sizing_tape_sizes_recalc_born_at_fill\n");
    const auto bars = btc_1d_autumn_bars();
    const auto tape = size_tape();
    CHECK(tape.size() == 32);
    double cum = 0.0;
    int recalc_born = 0, ordinary = 0;
    int recalc_born_discriminating = 0, ordinary_discriminating = 0;
    for (const TapeRow& r : tape) {
        const double equity = 1e9 + cum;
        cum += r.net_pnl;
        if (std::strcmp(r.id, "C") == 0) continue;
        const int k = index_of_day(bars, r.entry_ts);
        CHECK(k > 0);
        if (k <= 0) continue;
        const double at_fill = floor5(0.1 * equity / r.entry_price);
        const double at_signal_close = floor5(0.1 * equity / tick2(bars[k - 1].close));
        const bool is_first_a = std::strcmp(r.id, "A") == 0 && r.trade == 1;
        const bool born_in_recalc = std::strcmp(r.id, "B") == 0
            || (std::strcmp(r.id, "A") == 0 && !is_first_a);
        if (born_in_recalc) {
            ++recalc_born;
            CHECK_NEAR(r.qty, at_fill, 1e-9);
            if (std::fabs(at_signal_close - at_fill) > 1e-9) ++recalc_born_discriminating;
        } else {
            ++ordinary;
            CHECK_NEAR(r.qty, at_signal_close, 1e-9);
            if (std::fabs(at_signal_close - at_fill) > 1e-9) ++ordinary_discriminating;
        }
    }
    CHECK(recalc_born == 16);
    CHECK(ordinary == 15);
    CHECK(recalc_born_discriminating == 7);   // B + six A rows
    CHECK(ordinary_discriminating == 12);
}

// ---------------------------------------------------------------------------
// Rule 2, the engine: the sizing sensor's first cycle. A at the 10-01 close
// fills at the 10-02 open (843.20593); the recalc's close("A") fills at that
// open; the next recalc's B fills at W1 = 118279.31 and is sized THERE
// (845.4564), not at the cursor O (843.2) nor the bar's close (829.7); the
// 10-03 close_all takes B out at the 10-04 open.
// ---------------------------------------------------------------------------
class SizingFirstCycleProbe final : public BtcProbe {
public:
    SizingFirstCycleProbe() : BtcProbe(true) {
        initial_capital_ = 1e9;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 10.0;
        pyramiding_ = 10;
        margin_long_ = 0.0;
        margin_short_ = 0.0;
    }
    void on_bar(const Bar& b) override {
        if (b.timestamp == day_ts(2025, 10, 1) && flat() && confirmed()) {
            strategy_entry("A", true);
        }
        if (b.timestamp == day_ts(2025, 10, 2)) {
            if (opentrades() == 1 && entry_id(0) == "A") strategy_close("A");
            if (flat()) strategy_entry("B", true);
        }
        if (b.timestamp == day_ts(2025, 10, 3) && !flat() && confirmed()) {
            strategy_close_all();
        }
    }
};

void test_engine_recalc_born_entry_sized_at_w1_fill() {
    std::printf("test_engine_recalc_born_entry_sized_at_w1_fill\n");
    const auto bars = btc_1d_autumn_bars();
    SizingFirstCycleProbe p;
    p.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(p.last_error().empty());
    dump_trades(p, "size first cycle");
    const auto tape = size_tape();
    CHECK(p.trade_count() == 2);
    for (int i = 0; i < std::min(2, p.trade_count()); ++i) {
        const TapeRow& r = tape[static_cast<size_t>(i)];
        const Trade& t = p.get_trade(i);
        CHECK(t.entry_id == r.id);
        CHECK(t.entry_time == r.entry_ts);
        CHECK(t.exit_time == r.exit_ts);
        CHECK_NEAR(t.entry_price, r.entry_price, 1e-6);
        CHECK_NEAR(t.exit_price, r.exit_price, 1e-6);
        CHECK_NEAR(t.qty, r.qty, 1e-9);
        CHECK_NEAR(t.pnl, r.net_pnl, std::max(0.5, std::fabs(r.net_pnl) * 1e-6));
    }
}

// ---------------------------------------------------------------------------
// The probe's own decisions (JOAT BTC@1D tape rows TV 3/4 and TV 9/10).
// ---------------------------------------------------------------------------
class JoatProbe : public BtcProbe {
public:
    JoatProbe(bool coof, double capital) : BtcProbe(coof) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 10.0;
        pyramiding_ = 1;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.01;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
    }
};

// TV 3 (short 11-05 -> Dynamic Exit at the 11-11 open) and TV 4, the Short
// the on-fill recalc places at that open: qty 0.09245 sized at the fill
// 106011.13 with equity 98025.606, NOT 0.0951 at the 11-11 close 103058.99.
class Joat1111Probe final : public JoatProbe {
public:
    using JoatProbe::JoatProbe;
    void on_bar(const Bar& b) override {
        if (b.timestamp == day_ts(2025, 11, 4) && flat() && trades() == 0) {
            strategy_entry("Short", false);
        }
        if (b.timestamp == day_ts(2025, 11, 10) && is_short_pos()) {
            strategy_close("Short", "Dynamic Exit");
        }
        if (b.timestamp == day_ts(2025, 11, 11) && flat() && trades() == 1) {
            strategy_entry("Short", false);
        }
        if (b.timestamp == day_ts(2025, 11, 26) && is_short_pos()
            && trades() == 1) {
            strategy_close("Short", "Dynamic Exit");
        }
    }
};

void test_joat_1111_recalc_entry_sized_at_open_fill() {
    std::printf("test_joat_1111_recalc_entry_sized_at_open_fill\n");
    const auto bars = btc_1d_autumn_bars();
    // Equity before TV 3 = 100000 - 1534.5328 (TV cumulative PnL after TV 2).
    Joat1111Probe p(true, 100000.0 - 1534.5328);
    p.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(p.last_error().empty());
    dump_trades(p, "JOAT 11-11");
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t3 = p.get_trade(0);
        CHECK(!t3.is_long);
        CHECK(t3.entry_time == day_ts(2025, 11, 5));
        CHECK_NEAR(t3.entry_price, 101497.23, 1e-6);
        CHECK_NEAR(t3.qty, 0.097, 1e-9);
        CHECK(t3.exit_time == day_ts(2025, 11, 11));
        CHECK_NEAR(t3.exit_price, 106011.13, 1e-6);
        CHECK_NEAR(t3.pnl, -439.86115, 1e-3);
        const Trade& t4 = p.get_trade(1);
        CHECK(!t4.is_long);
        CHECK(t4.entry_time == day_ts(2025, 11, 11));
        CHECK_NEAR(t4.entry_price, 106011.13, 1e-6);
        CHECK_NEAR(t4.qty, 0.09245, 1e-9);   // engine before: 0.0951
        CHECK(t4.exit_time == day_ts(2025, 11, 27));
        CHECK_NEAR(t4.exit_price, 90484.01, 1e-6);
        CHECK_NEAR(t4.pnl, 1433.6656, 1e-3);
    }
}

// TV 9 / TV 10 on 2026-02-25 (O 64058.15, L 63913.27, H 69988.83, C 67988.04):
// the carried Dynamic Exit of TV 8 fills at O; the recalc's Short (TV 9) fills
// at O (0.16029); its recalc's Dynamic Exit is mid-bar and fills at W1; that
// fill's recalc places the next Short (TV 10) which fills at W2 = 69988.83 and
// is sized THERE: 0.14674 (cursor W1 -> 0.16069, close -> 0.15106); its
// Dynamic Exit rolls to the 02-26 open 67988.04.
class Joat0225Probe final : public JoatProbe {
public:
    using JoatProbe::JoatProbe;
    void on_bar(const Bar& b) override {
        if (b.timestamp == day_ts(2026, 2, 22) && flat() && trades() == 0) {
            strategy_entry("Short", false);                 // TV 8
        }
        if (b.timestamp == day_ts(2026, 2, 24) && is_short_pos()
            && trades() == 0) {
            strategy_close("Short", "Dynamic Exit");        // fills 02-25 O
        }
        if (b.timestamp == day_ts(2026, 2, 25)) {
            if (flat() && trades() == 1) {
                strategy_entry("Short", false);             // TV 9, at O
            } else if (is_short_pos() && trades() == 1) {
                strategy_close("Short", "Dynamic Exit");    // -> W1
            } else if (flat() && trades() == 2) {
                strategy_entry("Short", false);             // TV 10, at W2
            } else if (is_short_pos() && trades() == 2) {
                strategy_close("Short", "Dynamic Exit");    // -> 02-26 O
            }
        }
    }
};

void test_joat_0225_cascade_entry_sized_at_w2_fill() {
    std::printf("test_joat_0225_cascade_entry_sized_at_w2_fill\n");
    const auto bars = btc_1d_feb_bars();
    // Equity after TV 7 = 100000 + 2155.1975.
    Joat0225Probe p(true, 100000.0 + 2155.1975);
    p.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(p.last_error().empty());
    dump_trades(p, "JOAT 02-25");
    CHECK(p.trade_count() == 3);
    if (p.trade_count() == 3) {
        const Trade& t8 = p.get_trade(0);
        CHECK(t8.entry_time == day_ts(2026, 2, 23));
        CHECK_NEAR(t8.entry_price, 67643.39, 1e-6);
        CHECK_NEAR(t8.qty, 0.151, 1e-9);
        CHECK(t8.exit_time == day_ts(2026, 2, 25));
        CHECK_NEAR(t8.exit_price, 64058.15, 1e-6);
        CHECK_NEAR(t8.pnl, 539.38257, 1e-3);
        const Trade& t9 = p.get_trade(1);
        CHECK(t9.entry_time == day_ts(2026, 2, 25));
        CHECK_NEAR(t9.entry_price, 64058.15, 1e-6);
        CHECK_NEAR(t9.qty, 0.16029, 1e-9);
        CHECK(t9.exit_time == day_ts(2026, 2, 25));
        CHECK_NEAR(t9.exit_price, 63913.27, 1e-6);
        CHECK_NEAR(t9.pnl, 21.171562, 1e-3);
        const Trade& t10 = p.get_trade(2);
        CHECK(t10.entry_time == day_ts(2026, 2, 25));
        CHECK_NEAR(t10.entry_price, 69988.83, 1e-6);
        CHECK_NEAR(t10.qty, 0.14674, 1e-9);   // engine before: 0.15106
        CHECK(t10.exit_time == day_ts(2026, 2, 26));
        CHECK_NEAR(t10.exit_price, 67988.04, 1e-6);
        CHECK_NEAR(t10.pnl, 291.57126, 1e-3);
    }
}

// ---------------------------------------------------------------------------
// Controls.
// ---------------------------------------------------------------------------

// COOF off: the same decisions are ordinary close-calc placements — frozen at
// the signal close, filled at the next open. Byte-identical to before.
class OrdinaryProbe final : public JoatProbe {
public:
    using JoatProbe::JoatProbe;
    void on_bar(const Bar& b) override {
        if (b.timestamp == day_ts(2025, 11, 4) && flat() && trades() == 0) {
            strategy_entry("Short", false);
        }
        if (b.timestamp == day_ts(2025, 11, 10) && is_short_pos()) {
            strategy_close("Short", "Dynamic Exit");
        }
        if (b.timestamp == day_ts(2025, 11, 11) && flat() && trades() == 1) {
            strategy_entry("Short", false);   // signal close 103058.99
        }
        if (b.timestamp == day_ts(2025, 11, 26) && is_short_pos()
            && trades() == 1) {
            strategy_close("Short", "Dynamic Exit");
        }
    }
};

void test_coof_off_keeps_signal_close_freeze() {
    std::printf("test_coof_off_keeps_signal_close_freeze\n");
    const auto bars = btc_1d_autumn_bars();
    OrdinaryProbe p(false, 100000.0 - 1534.5328);
    p.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(p.last_error().empty());
    dump_trades(p, "COOF off");
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t = p.get_trade(1);
        // Placed at the 11-11 close, fills at the 11-12 open, frozen at
        // tick(close 11-11) = 103058.99 on equity 98025.606:
        // floor5(9802.5606 / 1.0001 / 103058.99) = 0.0951.
        CHECK(t.entry_time == day_ts(2025, 11, 12));
        CHECK_NEAR(t.entry_price, 103059.0, 1e-6);
        CHECK_NEAR(t.qty, 0.0951, 1e-9);
    }
}

// Inside a COOF run, a placement made by the ORDINARY close execution (not a
// fill recalc) still freezes at the signal close: the 11-04 Short above is
// exactly that (0.097 at tick(close 11-04) = 101497.22 on 98465.4672), and so
// is a fresh entry placed on a bar with no fill at all.
class CloseCalcInsideCoofProbe final : public JoatProbe {
public:
    using JoatProbe::JoatProbe;
    void on_bar(const Bar& b) override {
        if (b.timestamp == day_ts(2025, 12, 3) && flat() && trades() == 0) {
            strategy_entry("Long", true);   // close 93429.95 -> fills 12-04 open
        }
        if (b.timestamp == day_ts(2025, 12, 9) && is_long_pos()) {
            strategy_close("Long", "Dynamic Exit");
        }
    }
};

void test_close_calc_placement_inside_coof_still_freezes() {
    std::printf("test_close_calc_placement_inside_coof_still_freezes\n");
    const auto bars = btc_1d_autumn_bars();
    CloseCalcInsideCoofProbe p(true, 100000.0);
    p.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(p.last_error().empty());
    dump_trades(p, "close-calc inside COOF");
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.entry_time == day_ts(2025, 12, 4));
        CHECK_NEAR(t.entry_price, 93429.95, 1e-6);
        // floor5(10000 / 1.0001 / 93429.95) = 0.10702 (frozen at the 12-03
        // close 93429.95; the 12-04 open is the same print on BTC).
        CHECK_NEAR(t.qty, 0.10702, 1e-9);
        CHECK(t.exit_time == day_ts(2025, 12, 10));
    }
}

// An intrabar (limit) fill's recalc sees the finals as well, and a default-
// sized entry it places is sized at its own (cascade) fill: the 10-04 LL fill
// at 121620.84 on the O->L leg, then a percent_of_equity Long born in that
// recalc fills at W1 = 121510 with qty = floor5(10% x 1e9 / 121510).
class IntrabarRecalcSizingProbe final : public BtcProbe {
public:
    IntrabarRecalcSizingProbe() : BtcProbe(true) {
        initial_capital_ = 1e9;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 10.0;
        pyramiding_ = 10;
        margin_long_ = 0.0;
        margin_short_ = 0.0;
    }
    Bar recalc_bar{};
    bool recalc_seen = false;
    void on_bar(const Bar& b) override {
        if (b.timestamp == day_ts(2025, 10, 3) && flat() && confirmed()) {
            strategy_order("LL", true, 1.0, b.close * 0.995);   // 121620.84
        }
        if (b.timestamp == day_ts(2025, 10, 4) && opentrades() == 1
            && entry_id(0) == "LL" && !recalc_seen) {
            recalc_seen = true;
            recalc_bar = b;
            strategy_entry("R", true);
        }
        if (b.timestamp == day_ts(2025, 10, 6) && !flat() && confirmed()) {
            strategy_close_all();
        }
    }
};

void test_intrabar_fill_recalc_context_and_sizing() {
    std::printf("test_intrabar_fill_recalc_context_and_sizing\n");
    const auto bars = btc_1d_autumn_bars();
    IntrabarRecalcSizingProbe p;
    p.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(p.last_error().empty());
    dump_trades(p, "intrabar recalc");
    CHECK(p.recalc_seen);
    // The recalc's bar is the 10-04 bar's finals, not the path so far.
    CHECK_NEAR(p.recalc_bar.high, 122800.0, 1e-6);
    CHECK_NEAR(p.recalc_bar.low, 121510.0, 1e-6);
    CHECK_NEAR(p.recalc_bar.close, 122391.0, 1e-6);
    CHECK_NEAR(p.recalc_bar.volume, 8208.16678, 1e-6);
    bool found = false;
    for (int i = 0; i < p.trade_count(); ++i) {
        const Trade& t = p.get_trade(i);
        if (t.entry_id == "R") {
            found = true;
            CHECK(t.entry_time == day_ts(2025, 10, 4));
            CHECK_NEAR(t.entry_price, 121510.0, 1e-6);
            // Fill-time equity: 1e9 plus the open LL lot marked at the fill
            // (121510 - 121620.84 = -110.84) -> floor5(10% x 999999889.16 /
            // 121510) = 822.97744.
            const double equity = 1e9 + (121510.0 - 121620.84);
            CHECK_NEAR(t.qty, std::floor(0.1 * equity / 121510.0 * 1e5) / 1e5, 1e-6);
        }
    }
    CHECK(found);
}

}  // namespace

int main(int argc, char** argv) {
    g_dump = argc > 1 && std::strcmp(argv[1], "--dump") == 0;
    test_context_tape_decodes_to_bar_finals();
    test_engine_recalc_context_matches_tape();
    test_sizing_tape_sizes_recalc_born_at_fill();
    test_engine_recalc_born_entry_sized_at_w1_fill();
    test_joat_1111_recalc_entry_sized_at_open_fill();
    test_joat_0225_cascade_entry_sized_at_w2_fill();
    test_coof_off_keeps_signal_close_freeze();
    test_close_calc_placement_inside_coof_still_freezes();
    test_intrabar_fill_recalc_context_and_sizing();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
