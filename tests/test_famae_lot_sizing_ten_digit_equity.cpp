/*
 * test_famae_lot_sizing_ten_digit_equity.cpp — round 10 family AE
 * (stevenygabbyperez-fast-scalper-with-stops on NASDAQ:AAPL@15): family R's
 * rule 1 is not confined to sub-unit lots. TradingView sizes a default
 * percent_of_equity market entry on an integer-share, cent-priced book as
 *
 *     Q = floor_raw( sig10(E_s) / tick(close_S) )
 *
 * — the equity rounded to ten significant digits, divided in doubles, the
 * quotient floored with NO representation nudge. The engine sized these
 * instruments from the raw float-accumulated ledger with apply_qty_step's
 * 1e-6 nudge; both differ from TradingView exactly when the decimal quotient
 * is an integer (the probe's 2025-09-05 16:15Z reversal: E_s 1094521.68 =
 * 4584 x 238.77, TV 4583 shares, engine 4584 — one share the account cannot
 * pay at the 238.78 fill, so the reversal was declined where TV admits it).
 *
 * TradingView is ground truth: nine `lab tv` capital sweeps on NASDAQ:AAPL
 * 15m (campaign note log-20260905t223336z-67f0f181, tapes famae-sz-*),
 * time-gated from-flat strategy.entry('Long') with percent_of_equity 100,
 * margin 100/100, commission 0, fill at the next open, strategy.close an
 * hour later. Registry feed bars via `lab bars`.
 *
 *   a  signal 2025-07-03 16:45Z close 213.58, fill 07-07 13:30Z open 212.72
 *        C 897890.32      (= 4204 x 213.58; quotient 4203.999999999999) -> 4203
 *        C 897890.3200004 (raw quotient 4204.0000000019)                -> 4203
 *        C 897890.321                                                   -> 4204
 *        C 897890.319                                                   -> 4203
 *   b  signal 2025-07-09 19:45Z close 211.11, fill 07-10 13:30Z open 210.43
 *        C 887295.33      (= 4203 x 211.11)                             -> 4202
 *        C 887295.3300004                                               -> 4202
 *        C 887295.331                                                   -> 4203
 *   p  signal 2025-09-05 16:15Z close 238.77, fill 16:30Z open 238.78
 *        C 1094521.68     (the probe's E_s to the cent)                 -> 4583
 *        C 1094521.681    (Q 4584, 4584 x 238.78 = 1094567.52 > C)     -> no row
 *
 * The exact fill-price admission outside tv_money_scope (rules 2 and 5 stay
 * scoped) is what drops the last one, as it dropped the probe's 07-31 17:45Z
 * reversal on TradingView and in the engine alike.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "oracle_fixture_config_shim.hpp"

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

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

Bar mk(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

// lab bars NASDAQ:AAPL 15 --around 2025-07-03T16:45:00Z -n 6
static const std::vector<Bar> kBarsA = {
    mk(1751556600000LL, 213.89, 214.61, 213.67, 214.61),   // 2025-07-03 15:30Z
    mk(1751557500000LL, 214.59, 214.65, 214.21, 214.33),   // 2025-07-03 15:45Z
    mk(1751558400000LL, 214.33, 214.49, 214.05, 214.19),   // 2025-07-03 16:00Z
    mk(1751559300000LL, 214.19, 214.49, 213.96, 214.1),    // 2025-07-03 16:15Z
    mk(1751560200000LL, 214.1, 214.14, 213.78, 213.87),    // 2025-07-03 16:30Z
    mk(1751561100000LL, 213.85, 213.93, 213.32, 213.58),   // 2025-07-03 16:45Z  signal
    mk(1751895000000LL, 212.72, 216.23, 212.65, 213.26),   // 2025-07-07 13:30Z  fill @212.72
    mk(1751895900000LL, 213.26, 213.39, 212.47, 212.93),   // 2025-07-07 13:45Z
    mk(1751896800000LL, 212.94, 213.12, 212, 212.29),      // 2025-07-07 14:00Z
    mk(1751897700000LL, 212.29, 212.75, 211.805, 212.39),  // 2025-07-07 14:15Z
    mk(1751898600000LL, 212.36, 212.39, 211.83, 211.97),   // 2025-07-07 14:30Z
    mk(1751899500000LL, 211.97, 212.335, 211.6, 212.11),   // 2025-07-07 14:45Z
};
const int kSigA = 5;
const int kFillA = 6;

// lab bars NASDAQ:AAPL 15 --around 2025-07-09T19:45:00Z -n 6
static const std::vector<Bar> kBarsB = {
    mk(1752085800000LL, 209.73, 209.88, 209.62, 209.66),   // 2025-07-09 18:30Z
    mk(1752086700000LL, 209.65, 210.06, 209.65, 209.945),  // 2025-07-09 18:45Z
    mk(1752087600000LL, 209.95, 210.18, 209.65, 209.81),   // 2025-07-09 19:00Z
    mk(1752088500000LL, 209.82, 210, 209.71, 209.89),      // 2025-07-09 19:15Z
    mk(1752089400000LL, 209.86, 210.325, 209.74, 210.31),  // 2025-07-09 19:30Z
    mk(1752090300000LL, 210.3, 211.33, 210.195, 211.11),   // 2025-07-09 19:45Z  signal
    mk(1752154200000LL, 210.43, 211.14, 210.03, 210.59),   // 2025-07-10 13:30Z  fill @210.43
    mk(1752155100000LL, 210.585, 211.28, 210.51, 211.27),  // 2025-07-10 13:45Z
    mk(1752156000000LL, 211.27, 211.85, 210.71, 211.37),   // 2025-07-10 14:00Z
    mk(1752156900000LL, 211.4, 212.29, 211.33, 212.01),    // 2025-07-10 14:15Z
    mk(1752157800000LL, 212.03, 213.46, 211.92, 213.345),  // 2025-07-10 14:30Z
    mk(1752158700000LL, 213.38, 213.48, 212.03, 212.36),   // 2025-07-10 14:45Z
};
const int kSigB = 5;
const int kFillB = 6;

// lab bars NASDAQ:AAPL 15 --around 2025-09-05T16:15:00Z -n 6
static const std::vector<Bar> kBarsP = {
    mk(1757084400000LL, 238.99, 239.5, 238.79, 239.44),    // 2025-09-05 15:00Z
    mk(1757085300000LL, 239.42, 240.17, 239.39, 239.8),    // 2025-09-05 15:15Z
    mk(1757086200000LL, 239.84, 240.32, 239.81, 240.05),   // 2025-09-05 15:30Z
    mk(1757087100000LL, 240.04, 240.06, 238.92, 238.955),  // 2025-09-05 15:45Z
    mk(1757088000000LL, 238.98, 239.26, 238.72, 239.09),   // 2025-09-05 16:00Z
    mk(1757088900000LL, 239.08, 239.1, 238.52, 238.77),    // 2025-09-05 16:15Z  signal
    mk(1757089800000LL, 238.78, 239.43, 238.5, 239.43),    // 2025-09-05 16:30Z  fill @238.78
    mk(1757090700000LL, 239.41, 239.72, 238.9, 239.33),    // 2025-09-05 16:45Z
    mk(1757091600000LL, 239.3, 239.73, 239.165, 239.55),   // 2025-09-05 17:00Z
    mk(1757092500000LL, 239.58, 239.67, 239.35, 239.64),   // 2025-09-05 17:15Z
    mk(1757093400000LL, 239.65, 239.65, 238.9, 239.01),    // 2025-09-05 17:30Z
    mk(1757094300000LL, 239.01, 239.1, 238.74, 238.875),   // 2025-09-05 17:45Z
};
const int kSigP = 5;
const int kFillP = 6;

// The sensors' account: NASDAQ:AAPL (mintick 0.01, one-share lots), the
// declared initial_capital, percent_of_equity 100, commission 0, margin
// 100/100, slippage 0, market fills at the next open, margin calls on.
class Sensor : public pineforge::source::PineStrategyHost {
public:
    Sensor(double capital, int signal_bar, int close_bar)
        : signal_bar_(signal_bar), close_bar_(close_bar) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
        process_orders_on_close_ = false;
        syminfo_mintick_ = 0.01;
        qty_step_ = 1.0;
        syminfo_.pointvalue = 1.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        margin_call_enabled_ = true;
    }
    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == signal_bar_) strategy_entry("Long", true);
        if (bar_index_ == close_bar_ && signed_position_size() > 0.0) {
            strategy_close_all();
        }
    }
    int trades() const { return trade_count(); }
    double e_price(int i) const { return closed_trade_entry_price(i); }
    double t_size(int i) const { return closed_trade_size(i); }
    int e_bar(int i) const { return closed_trade_entry_bar_index(i); }
    double position() const { return signed_position_size(); }
private:
    int signal_bar_;
    int close_bar_;
};

bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

struct Pin {
    const char* tape;
    const std::vector<Bar>* bars;
    int signal_bar;
    int fill_bar;
    double capital;
    double want_qty;      // 0 = the order is dropped: no row, flat
};

const Pin kPins[] = {
    {"famae-sz-a1", &kBarsA, kSigA, kFillA, 897890.32,      4203.0},
    {"famae-sz-a2", &kBarsA, kSigA, kFillA, 897890.3200004, 4203.0},
    {"famae-sz-a3", &kBarsA, kSigA, kFillA, 897890.321,     4204.0},
    {"famae-sz-a4", &kBarsA, kSigA, kFillA, 897890.319,     4203.0},
    {"famae-sz-b1", &kBarsB, kSigB, kFillB, 887295.33,      4202.0},
    {"famae-sz-b2", &kBarsB, kSigB, kFillB, 887295.3300004, 4202.0},
    {"famae-sz-b3", &kBarsB, kSigB, kFillB, 887295.331,     4203.0},
    {"famae-sz-p1", &kBarsP, kSigP, kFillP, 1094521.68,     4583.0},
    {"famae-sz-p2", &kBarsP, kSigP, kFillP, 1094521.681,    0.0},
};

void test_tape_replays() {
    std::printf("-- NASDAQ:AAPL: floor_raw(sig10(E_s) / tick(close_S)) on one-share lots --\n");
    for (const Pin& pin : kPins) {
        const std::vector<Bar>& bars = *pin.bars;
        Sensor s(pin.capital, pin.signal_bar, pin.signal_bar + 5);
        s.run(bars.data(), (int)bars.size());
        std::printf("   %s  C %.7f  trades %d  qty %.0f\n", pin.tape, pin.capital,
                    s.trades(), s.trades() > 0 ? s.t_size(0) : 0.0);
        if (pin.want_qty > 0.0) {
            CHECK(s.trades() == 1);
            if (s.trades() == 1) {
                CHECK(near(s.t_size(0), pin.want_qty));
                CHECK(s.e_bar(0) == pin.fill_bar);
                CHECK(near(s.e_price(0), bars[pin.fill_bar].open, 1e-9));
            }
        } else {
            CHECK(s.trades() == 0);
            CHECK(near(s.position(), 0.0));
        }
    }
}

}  // namespace

int main() {
    test_tape_replays();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
