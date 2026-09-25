// R5 wave H, lane H-MEASURE, row E19 / G2-22: the excursion model, measured
// against TradingView.
//
// RULING A48 keeps per-lot MFE/MAE in the Pine source host
// (PineStrategyHost::owns_lot_excursions, closed_lot_excursion /
// owner_lot_excursion in src/source/pine_strategy_host.cpp, the H/L/C walk
// sample_open_trade_extremes with its entry-bar masks), while the kernel keeps
// a sampler of its own for every other host (NativeExecutionConsumer::
// apply_excursion at each delivered path point, and the closing-row fold in
// build_close_trade_with_costs). The ruling was pinned against ab9714be's
// rows (tests/test_l11a_host_excursion_twin.cpp); this row pins both models
// against TradingView's own tape.
//
// The tape: `lab tv` export hm-e19-stop-reversal-grouping (ws-report-v1,
// rangeProof covered) of corpus probe order-stop-entry-reversal-grouping-01
// ("PF probe 57", BINANCE:ETHUSDT.P 15m, 2025-04-01..2026-05-01, 1580
// trades); its excursion cells equal the corpus tape's on all 1460 trades the
// two share. Three of its days are replayed here on the corpus 15m feed
// (fixtures/e19_excursion_tape/bars.inc), each through
//   - the Pine adapter: a PineStrategyHost running the probe's script, and
//   - the kernel: a bare NativeStrategyHost submitting the same requests
//     (market lots, a stop reversal at the prior bar's high, a stop reversal
//     at the prior bar's low, a flatten), so every fill is the same fill;
// and every closed row is compared with the tape's (fixtures/.../tv_rows.inc).
//
// What the lane's corpus-wide measurement found (both models run over the 312
// probes: 431 390 trades, the two agree on all but 341; of the 326 of those a
// TradingView export identifies, the kernel's number is TradingView's on 315
// and the adapter's on none) is what these rows pin, one of each class:
//   A. entry-bar over-mask: a stop entry that fills at the bar's open has no
//      path before it, yet the adapter masks the entry bar's adverse or
//      favorable extreme (day 0 LREV: adverse 0 where TradingView has -3.22;
//      day 1 SREV: favorable 1.47 where TradingView has 6.97);
//   B. exit-bar over-fold: a lot closed at the exit bar's open still takes
//      that bar's later extreme (day 1 L1/L2: adverse -16.35 / -3.44 where
//      TradingView has -13.75 / -0.43, the entry bars' lows);
//   C. an open print ahead of a same-bar stop fill: TradingView counts the
//      exit bar's open (3925.80) in the favorable excursion; neither model
//      samples a bar's open print, so both report the fill alone here
//      (13.64 / 7.78, day 2 L1/L2) against TradingView's 13.65 / 7.79. (Under
//      the Pine run's own price grid the kernel sampler reads the stop's
//      half-tick crossing instead, 13.645 -- the "overshoot" the l11a twin
//      retired in favour of ab9714be's 13.64; TradingView has neither.)
// The other six rows are controls: all three agree.
//   D. a calc_on_order_fills scratch (second tape, hm-e19-coof-scratch: a
//      long market entry fills at the 00:30 open and its own fill
//      recalculation closes it at the same print): TradingView reports 0 / 0
//      on all four days, and so does the kernel; the adapter folds the whole
//      entry bar into the lot (ab9714be pine_scheduler.cpp:357-366's
//      update_per_trade_extremes on every fill recalculation).
//
// The adapter columns were pinned as MEASURED DIVERGENCES, to flip when a lane
// moved the Pine host onto the kernel sampler.
//
// expectation corrected (R5 lane H-THIN, E19): that lane is this one. The Pine
// host no longer owns lot excursions (owns_lot_excursions is the kernel's
// default, false), so the kernel samples its lots and the adapter columns are
// TradingView's wherever the kernel's are: classes A and B and all four COOF
// scratches flip to the tape (adapter == TradingView 6 -> 10 of 12, 0 -> 4 of
// 4). Class C stays off the tape on both sides, and the Pine run shows the
// kernel sampler's reading under the adapter's per-kind tick rules: the
// stop's half-tick crossing, 13.645 / 7.785 (was 13.64 / 7.78), 0.005 closer
// to TradingView's 13.65 / 7.79.
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(x) do { \
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};
struct TapeRow {
    int day;
    int trade;
    bool is_long;
    std::int64_t entry_ms, exit_ms;
    double entry, exit, qty, favorable, adverse;  // TradingView: adverse <= 0
};

#include "fixtures/e19_excursion_tape/bars.inc"
#include "fixtures/e19_excursion_tape/tv_rows.inc"

template <std::size_t N>
std::vector<Bar> day_bars(const FeedBar (&rows)[N]) {
    std::vector<Bar> bars;
    for (const FeedBar& r : rows) bars.push_back({r.open, r.high, r.low, r.close, r.volume, r.ts});
    return bars;
}

int minute_of_day(std::int64_t ms) {
    return static_cast<int>(((ms / 60000) % 1440 + 1440) % 1440);
}

// PF probe 57 as generated code runs it (strategy.pine, the probe's own
// source): the chart is UTC, so `hour`/`minute` are the bar open's.
class Probe57 final : public source::PineStrategyHost {
public:
    Probe57() {
        source::PineStrategyConfig c;
        c.initial_capital = 1000000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        c.pyramiding = 2;
        c.process_orders_on_close = false;
        c.commission_value = 0.0;
        c.slippage = 0;
        configure_pine_strategy(c);
        set_syminfo_mintick(0.01);
    }
    void on_source_bar(const Bar& bar) override {
        const int m = minute_of_day(current_bar_.timestamp);
        const double pos = live_position_size();
        if (m == 15 && pos == 0.0) strategy_entry("L1", true, kNaN, kNaN, 1.0, "long lot 1");
        if (m == 30 && pos > 0.0) strategy_entry("L2", true, kNaN, kNaN, 1.0, "long lot 2");
        if (m == 45 && pos > 0.0)
            strategy_entry("SREV", false, kNaN, bar.high, 1.0, "short stop reversal");
        if (m == 75 && pos < 0.0)
            strategy_entry("LREV", true, kNaN, bar.low, 1.0, "long stop reversal");
        if (m == 105 && pos != 0.0) strategy_close_all();
    }
};

// The same requests on a bare host: the kernel's own excursion model.
class KernelProbe final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar& bar, const NativeDecisionContext& ctx) override {
        const int m = minute_of_day(ctx.script_bar_open_ms);
        const double pos = physical_position().signed_units;
        if (m == 15 && pos == 0.0) (void)submit({no::Transact{1.0}, "L1", "long lot 1"});
        if (m == 30 && pos > 0.0) (void)submit({no::Transact{1.0}, "L2", "long lot 2"});
        if (m == 45 && pos > 0.0)
            (void)submit({no::Transact{-(pos + 1.0)}, "SREV", "short stop reversal",
                          no::Stop{bar.high}});
        if (m == 75 && pos < 0.0)
            (void)submit({no::Transact{-pos + 1.0}, "LREV", "long stop reversal",
                          no::Stop{bar.low}});
        if (m == 105 && pos != 0.0) (void)submit({no::Flatten{}, "flat", ""});
    }
};

NativeRunSpec kernel_spec(int day) {
    NativeRunSpec s;
    s.identity = {"e19-tape-day-" + std::to_string(day), 1};
    s.input_tf = "15";
    s.script_tf = "15";
    s.ticker = "ETHUSDT.P";
    s.tickerid = "BINANCE:ETHUSDT.P";
    s.type = "crypto";
    s.currency = "USDT";
    s.basecurrency = "ETH";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 1000000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::Percent;
    s.fee_value = 0.0;
    return s;
}

// The measured numbers, per tape row: {adapter favorable, adapter adverse,
// kernel favorable, kernel adverse} as closed-row magnitudes (>= 0).
struct Measured {
    int trade;
    double a_fav, a_adv, k_fav, k_adv;
    const char* cls;
};
const Measured kMeasured[] = {
    {29, 4.98, 10.37, 4.98, 10.37, "control"},
    {30, 0.32, 12.67, 0.32, 12.67, "control"},
    {31, 3.58, 25.21, 3.58, 25.21, "control"},
    {32, 27.07, 3.22, 27.07, 3.22, "A entry-bar over-mask (adverse)"},
    {169, 0.14, 13.75, 0.14, 13.75, "B exit-bar over-fold (adverse)"},
    {170, 11.55, 0.43, 11.55, 0.43, "B exit-bar over-fold (adverse)"},
    {171, 6.97, 13.62, 6.97, 13.62, "A entry-bar over-mask (favorable)"},
    {172, 4.50, 21.77, 4.50, 21.77, "control"},
    {797, 13.645, 5.77, 13.64, 5.77, "C open print before a same-bar stop fill"},
    {798, 7.785, 6.01, 7.78, 6.01, "C open print before a same-bar stop fill"},
    {799, 5.69, 22.27, 5.69, 22.27, "control"},
    {800, 3.08, 24.92, 3.08, 24.92, "control"},
};

// ── D. the calc_on_order_fills scratch ────────────────────────────────────

class CoofScratch final : public source::PineStrategyHost {
public:
    CoofScratch() {
        source::PineStrategyConfig c;
        c.initial_capital = 1000000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        c.pyramiding = 1;
        c.calc_on_order_fills = true;
        configure_pine_strategy(c);
        set_syminfo_mintick(0.01);
    }
    void on_source_bar(const Bar&) override {
        if (minute_of_day(current_bar_.timestamp) == 15 && live_position_size() == 0.0)
            strategy_entry("L", true);
        if (live_position_size() > 0.0) strategy_close("L");
    }
};

// The kernel's scratch: the fill's own callback closes the lot at the same
// point through execute_current.
class KernelScratch final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext& ctx) override {
        if (minute_of_day(ctx.script_bar_open_ms) == 15 && physical_position().signed_units == 0.0)
            (void)submit({no::Transact{1.0}, "L", ""});
    }
    void on_native_applied(const no::ExecutionAppliedEvent& e, const NativeDecisionContext&) override {
        if (e.opened_units <= 0.0 || closing_) return;
        closing_ = true;
        const auto r = submit({no::Flatten{}, "close", ""});
        if (r.handle) closed_ = std::holds_alternative<no::ExecutionAppliedEvent>(
            execute_current(NativeCurrentExecution{*r.handle}));
        closing_ = false;
    }
    bool closed_ = false;
private:
    bool closing_ = false;
};

void test_coof_scratch() {
    const std::vector<Bar> days[] = {day_bars(kCoofDay0), day_bars(kCoofDay1),
                                     day_bars(kCoofDay2), day_bars(kCoofDay3)};
    // The adapter's magnitudes, pinned: the kernel's, 0 / 0 (were the entry
    // bar's extremes folded in: {4.08, 4.21}, {11.75, 0.0}, {0.0, 11.19},
    // {6.44, 0.22}).
    const double adapter[4][2] = {{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}};
    int adapter_tv = 0, kernel_tv = 0;
    for (const TapeRow& tv : kCoofTape) {
        const int d = tv.day;
        CoofScratch pine;
        pine.run(days[d].data(), static_cast<int>(days[d].size()), "15", "15");
        KernelScratch kernel;
        CHECK(kernel.configure_native(kernel_spec(10 + d)).status == NativeSetupStatus::Applied);
        kernel.run(days[d].data(), static_cast<int>(days[d].size()));
        CHECK(pine.last_error().empty());
        CHECK(kernel.last_error().empty());
        CHECK(kernel.closed_);
        CHECK(pine.trade_count() == 1);
        CHECK(kernel.trade_count() == 1);
        if (pine.trade_count() != 1 || kernel.trade_count() != 1) continue;
        const Trade& a = pine.get_trade(0);
        const Trade& x = kernel.get_trade(0);
        for (const Trade* t : {&a, &x}) {
            CHECK(t->entry_time == tv.entry_ms);
            CHECK(near(t->entry_price, tv.entry));
            CHECK(near(t->exit_price, tv.exit));
        }
        // Same fill, same price. The Pine row is dated at the chart bar's open,
        // as TradingView dates it; the bare host's current execution is dated
        // at its point, the end of that bar (design doc section 3.7's note:
        // bare hosts date rows on the execution point, the adapter on the bar).
        CHECK(a.exit_time == tv.exit_ms);
        CHECK(x.exit_time == tv.exit_ms + 900000);
        const bool a_is_tv = near(a.max_runup, tv.favorable) && near(a.max_drawdown, -tv.adverse);
        const bool k_is_tv = near(x.max_runup, tv.favorable) && near(x.max_drawdown, -tv.adverse);
        adapter_tv += a_is_tv;
        kernel_tv += k_is_tv;
        std::printf("coof day%d #%d L %.2f->%.2f  TV %.3f/%.3f | adapter %.3f/%.3f%s | kernel %.3f/%.3f%s  [D coof scratch]\n",
                    d, tv.trade, tv.entry, tv.exit, tv.favorable, -tv.adverse, a.max_runup, a.max_drawdown,
                    a_is_tv ? " =TV" : " !=TV", x.max_runup, x.max_drawdown, k_is_tv ? " =TV" : " !=TV");
        CHECK(near(a.max_runup, adapter[d][0]));
        CHECK(near(a.max_drawdown, adapter[d][1]));
        CHECK(a_is_tv);
        CHECK(k_is_tv);
    }
    std::printf("coof scratch rows 4: adapter == TradingView %d, kernel == TradingView %d\n", adapter_tv, kernel_tv);
    CHECK(adapter_tv == 4);
    CHECK(kernel_tv == 4);
}

}  // namespace

int main() {
    const std::vector<Bar> days[] = {day_bars(kDay0), day_bars(kDay1), day_bars(kDay2)};
    std::vector<Trade> pine_rows[3];
    std::vector<Trade> kernel_rows[3];
    for (int d = 0; d < 3; ++d) {
        Probe57 pine;
        pine.run(days[d].data(), static_cast<int>(days[d].size()), "15", "15");
        CHECK(pine.last_error().empty());
        for (int i = 0; i < pine.trade_count(); ++i) pine_rows[d].push_back(pine.get_trade(i));

        KernelProbe kernel;
        CHECK(kernel.configure_native(kernel_spec(d)).status == NativeSetupStatus::Applied);
        kernel.run(days[d].data(), static_cast<int>(days[d].size()));
        CHECK(kernel.last_error().empty());
        for (int i = 0; i < kernel.trade_count(); ++i) kernel_rows[d].push_back(kernel.get_trade(i));
        CHECK(pine_rows[d].size() == 4);
        CHECK(kernel_rows[d].size() == 4);
    }

    int agree_adapter = 0, agree_kernel = 0, n = 0;
    int next[3] = {0, 0, 0};
    for (const TapeRow& tv : kTape) {
        const int d = tv.day;
        const int k = next[d]++;
        if (k >= static_cast<int>(pine_rows[d].size()) || k >= static_cast<int>(kernel_rows[d].size())) {
            CHECK(false);
            continue;
        }
        const Trade& a = pine_rows[d][static_cast<std::size_t>(k)];
        const Trade& x = kernel_rows[d][static_cast<std::size_t>(k)];
        const Measured* m = nullptr;
        for (const Measured& r : kMeasured)
            if (r.trade == tv.trade) m = &r;
        CHECK(m != nullptr);
        if (m == nullptr) continue;
        ++n;
        // The same physical trade on all three: side, both fills, both dates.
        for (const Trade* t : {&a, &x}) {
            CHECK(t->is_long == tv.is_long);
            CHECK(t->entry_time == tv.entry_ms);
            CHECK(t->exit_time == tv.exit_ms);
            CHECK(near(t->entry_price, tv.entry));
            CHECK(near(t->exit_price, tv.exit));
            CHECK(near(t->qty, tv.qty));
        }
        const double tv_fav = tv.favorable, tv_adv = -tv.adverse;
        const bool a_is_tv = near(a.max_runup, tv_fav) && near(a.max_drawdown, tv_adv);
        const bool k_is_tv = near(x.max_runup, tv_fav) && near(x.max_drawdown, tv_adv);
        agree_adapter += a_is_tv;
        agree_kernel += k_is_tv;
        std::printf("day%d #%d %s %.2f->%.2f  TV %.3f/%.3f | adapter %.3f/%.3f%s | kernel %.3f/%.3f%s  [%s]\n",
                    d, tv.trade, tv.is_long ? "L" : "S", tv.entry, tv.exit, tv_fav, tv_adv,
                    a.max_runup, a.max_drawdown, a_is_tv ? " =TV" : " !=TV",
                    x.max_runup, x.max_drawdown, k_is_tv ? " =TV" : " !=TV", m->cls);
        // Both models, pinned to what they measure today.
        CHECK(near(a.max_runup, m->a_fav));
        CHECK(near(a.max_drawdown, m->a_adv));
        CHECK(near(x.max_runup, m->k_fav));
        CHECK(near(x.max_drawdown, m->k_adv));
        const bool control = std::string(m->cls) == "control";
        const bool class_c = m->cls[0] == 'C';
        if (control) {
            CHECK(a_is_tv);
            CHECK(k_is_tv);
        } else if (class_c) {
            CHECK(!a_is_tv);
            CHECK(!k_is_tv);
        } else {
            // Classes A and B: the kernel sampler is TradingView's, and so is
            // the adapter since it reads the kernel's (was CHECK(!a_is_tv)).
            CHECK(a_is_tv);
            CHECK(k_is_tv);
        }
    }
    CHECK(n == 12);
    std::printf("tape rows %d: adapter == TradingView %d, kernel == TradingView %d\n",
                n, agree_adapter, agree_kernel);
    CHECK(agree_adapter == 10);
    CHECK(agree_kernel == 10);
    test_coof_scratch();
    std::printf("test_e19_excursion_tape: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
