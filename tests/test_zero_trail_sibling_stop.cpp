/*
 * test_zero_trail_sibling_stop.cpp — H-MEASURE row M16 (G2-18, the sibling stop).
 *
 * PineExecutionAdapter::exit() lowers a strategy.exit(trail_points=n,
 * trail_offset=0) that is ALREADY reached at its placement close to two legs
 * in one group: the kernel's Trail (TrailTicks 0.5, best_seed = the carried
 * best) and a sibling Stop at the carried best ("A sibling generic stop
 * preserves the next-open print decision"). A Stop is reached by a touch; a
 * zero-distance ride needs a move strictly past its best. They separate on a
 * bar that opens exactly ON the carried best.
 *
 * 1. The paired differential, seven synthetic tapes: the adapter as it stands,
 *    a bare NativeStrategyHost given the adapter's own accepted Trail alone,
 *    and the bare host given both legs. The bare host with both legs IS the
 *    adapter, bit for bit; the Trail alone rides past the open and fills on a
 *    later leg of the bar, 0.5 to 1.5 ticks away (Z1: 100.11500000000001 on
 *    the low leg against 100.09999999999999 at the open).
 *
 * 2. TradingView (`lab tv`, NYSE:F 15m, ws-report-v1, rangeProof covered;
 *    tests/fixtures/zero_trail_sibling_stop): eight longs and eight shorts of
 *    that shape. TradingView exits all sixteen on the bar that opens on the
 *    carried best, at the carried best's tick: the touch at the open. The
 *    adapter books TradingView's bar and price 16 of 16; the kernel's Trail
 *    alone books a different price 16 of 16.
 */

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

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

#ifndef PINEFORGE_ZERO_TRAIL_FIXTURE_DIR
#error "PINEFORGE_ZERO_TRAIL_FIXTURE_DIR must name tests/fixtures/zero_trail_sibling_stop"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kBarMs = 900'000;

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close;
};

#include "fixtures/zero_trail_sibling_stop/bars.inc"

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }
bool same_price(double a, double b) { return std::fabs(a - b) <= 1e-9; }

// ── the three runs ────────────────────────────────────────────────────

// Bar 0 places a market entry; the bar whose open fills it issues the
// zero-offset trailing exit at its close; a 25-bar timeout closes the rest.
class PineRun final : public source::PineStrategyHost {
public:
    PineRun(bool is_long, double trail_points, double qty)
        : is_long_(is_long), trail_points_(trail_points) {
        source::PineStrategyConfig c;
        c.initial_capital = 1'000'000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = qty;
        c.commission_value = 0.0;
        c.slippage = 0;
        c.pyramiding = 1;
        c.process_orders_on_close = false;
        configure_pine_strategy(c);
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        const char* id = is_long_ ? "L" : "S";
        const double units = physical_position().signed_units;
        if (bar_index_ == 0) strategy_entry(id, is_long_);
        if (units != 0.0 && entry_bar_ < 0) {
            entry_bar_ = bar_index_;
            strategy_exit(is_long_ ? "LX" : "SX", id, kNaN, kNaN, trail_points_, 0.0);
        }
        if (units != 0.0 && entry_bar_ >= 0 && bar_index_ - entry_bar_ >= 25)
            strategy_close(id, "timeout");
    }
    // The two legs the exit was accepted as.
    std::optional<no::Trail> trail;
    std::optional<no::Stop> stop;
    void collect() {
        const std::string x = is_long_ ? "LX" : "SX";
        for (const auto& ev : native_events(0)) {
            if (!ev.command) continue;
            const auto* a = std::get_if<no::AcceptedEvent>(&*ev.command);
            if (!a || a->request().label != x) continue;
            if (const auto* t = std::get_if<no::Trail>(&a->request().trigger)) trail = *t;
            if (const auto* s = std::get_if<no::Stop>(&a->request().trigger)) stop = *s;
        }
    }
private:
    bool is_long_;
    double trail_points_;
    int entry_bar_ = -1;
};

// The bare kernel: the same entry, then at bar 1's calculation the legs the
// adapter was accepted with (as Flatten closes: the trigger is the question).
class KernelRun final : public NativeStrategyHost {
public:
    KernelRun(bool is_long, double qty) : is_long_(is_long), qty_(qty) {}
    std::vector<no::Request> legs;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int b = bar_++;
        if (b == 0) (void)submit({no::Transact{is_long_ ? qty_ : -qty_}, "E", ""});
        if (b == 1) for (const auto& r : legs) (void)submit(r);
    }
private:
    bool is_long_;
    double qty_;
    int bar_ = 0;
};

NativeRunSpec kernel_spec() {
    NativeRunSpec s;
    s.identity = {"hm-m16", 1};
    s.event_retention = NativeEventRetention::Full;
    s.input_tf = "15";
    s.script_tf = "15";
    s.tickerid = "NYSE:F";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 1'000'000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    return s;
}

// The closing fill as the kernel recorded it.
struct Fill {
    bool filled = false;
    int bar = -1;
    double raw = kNaN;
    double resolved = kNaN;
    bool at_open = false;
    bool stop_leg = false;
    bool trail_leg = false;
};

Fill closing_fill(const NativeStrategyHost& host) {
    Fill f;
    for (const auto& ev : host.native_events(0)) {
        if (!ev.command) continue;
        const auto* ap = std::get_if<no::ExecutionAppliedEvent>(&*ev.command);
        if (!ap || ap->closed_units == 0.0 || f.filled) continue;
        f.filled = true;
        f.bar = ap->cursor.point.interval_index;
        f.raw = ap->raw_price;
        f.resolved = ap->resolved_price;
        f.at_open = ap->cursor.point.path_phase == NativePathPhase::Open;
        f.stop_leg = std::holds_alternative<no::Stop>(ap->request().trigger);
        f.trail_leg = std::holds_alternative<no::Trail>(ap->request().trigger);
    }
    return f;
}

struct Triple {
    Fill adapter, trail_alone, both_legs;
    double adapter_booked = kNaN;
    int adapter_exit_bar = -1;
    bool adapter_has_sibling = false;
};

Triple run_three(const std::vector<Bar>& bars, bool is_long, double trail_points, double qty) {
    Triple out;
    PineRun a(is_long, trail_points, qty);
    a.fixture_retain_all_events();
    a.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(a.last_error().empty());
    a.collect();
    out.adapter = closing_fill(a);
    out.adapter_has_sibling = a.trail.has_value() && a.stop.has_value();
    if (a.trade_count() >= 1) {
        out.adapter_booked = a.get_trade(0).exit_price;
        out.adapter_exit_bar = a.get_trade(0).exit_bar_index;
    }
    CHECK(a.trail.has_value());
    if (!a.trail) return out;
    for (int variant = 0; variant < 2; ++variant) {
        KernelRun k(is_long, qty);
        no::Request trail{no::Flatten{}, "x", ""};
        trail.trigger = *a.trail;
        k.legs.push_back(trail);
        if (variant == 1 && a.stop) {
            no::Request stop{no::Flatten{}, "x", ""};
            stop.trigger = *a.stop;
            k.legs.push_back(stop);
        }
        CHECK(k.configure_native(kernel_spec()).status == NativeSetupStatus::Applied);
        k.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(k.last_error().empty());
        (variant == 0 ? out.trail_alone : out.both_legs) = closing_fill(k);
    }
    return out;
}

// ── 1. the paired differential ─────────────────────────────────────────

Bar mk(int i, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, 1743379200000LL + static_cast<std::int64_t>(i) * kBarMs};
}

struct Synthetic {
    const char* key;
    bool is_long;
    std::vector<Bar> bars;
    // pinned: the adapter's booked price and fill, the Trail alone's fill
    double adapter_booked;
    double trail_alone_raw;
    bool trail_alone_same_bar;
};

void test_paired_differential() {
    std::printf("-- the paired differential (synthetic) --\n");
    const Synthetic cases[] = {
        {"Z1 long, close AT the activation, next open ON it", true,
         {mk(0, 100.00, 100.00, 100.00, 100.00), mk(1, 100.00, 100.10, 100.00, 100.10),
          mk(2, 100.10, 100.12, 100.05, 100.06), mk(3, 100.06, 100.07, 100.00, 100.01)},
         100.09999999999999, 100.11500000000001, true},
        {"Z2 long, close PAST the activation, next open ON it", true,
         {mk(0, 100.00, 100.00, 100.00, 100.00), mk(1, 100.00, 100.13, 100.00, 100.13),
          mk(2, 100.13, 100.15, 100.08, 100.09), mk(3, 100.09, 100.10, 100.00, 100.01)},
         100.13, 100.14500000000001, true},
        {"Z3 short mirror of Z1", false,
         {mk(0, 100.00, 100.00, 100.00, 100.00), mk(1, 100.00, 100.00, 99.90, 99.90),
          mk(2, 99.90, 99.95, 99.88, 99.94), mk(3, 99.94, 100.00, 99.93, 99.99)},
         99.900000000000006, 99.884999999999991, true},
        {"Z6 long, close in the activation's cell, next open ON the activation", true,
         {mk(0, 100.00, 100.00, 100.00, 100.00), mk(1, 100.00, 100.095, 100.00, 100.095),
          mk(2, 100.10, 100.12, 100.05, 100.06), mk(3, 100.06, 100.07, 100.00, 100.01)},
         100.09999999999999, 100.11500000000001, true},
        {"Z7 long, next open ON the best, low leg first", true,
         {mk(0, 100.00, 100.00, 100.00, 100.00), mk(1, 100.00, 100.10, 100.00, 100.10),
          mk(2, 100.10, 100.16, 100.08, 100.15), mk(3, 100.15, 100.16, 100.00, 100.01)},
         100.09999999999999, 100.09500000000001, true},
    };
    for (const auto& c : cases) {
        const Triple t = run_three(c.bars, c.is_long, 10.0, 1.0);
        std::printf("   %s: adapter %.17g (%s, open=%d) | Trail alone %.17g (open=%d) | both legs %.17g\n",
                    c.key, t.adapter_booked, t.adapter.stop_leg ? "Stop" : "Trail",
                    (int)t.adapter.at_open, t.trail_alone.raw, (int)t.trail_alone.at_open,
                    t.both_legs.raw);
        // The adapter rests the sibling, which is the leg that fills: at the
        // open, at the carried best.
        CHECK(t.adapter_has_sibling);
        CHECK(t.adapter.filled && t.adapter.bar == 2 && t.adapter.at_open && t.adapter.stop_leg);
        CHECK(same_bits(t.adapter_booked, c.adapter_booked));
        // The kernel given both legs IS the adapter's fill.
        CHECK(t.both_legs.filled && t.both_legs.bar == 2 && t.both_legs.at_open
              && t.both_legs.stop_leg && same_bits(t.both_legs.raw, t.adapter.raw));
        // The Trail alone does not fire on the touch: it rides and fills
        // later on the same bar, at another price.
        CHECK(t.trail_alone.filled && t.trail_alone.trail_leg && !t.trail_alone.at_open);
        CHECK((t.trail_alone.bar == 2) == c.trail_alone_same_bar);
        CHECK(same_bits(t.trail_alone.raw, c.trail_alone_raw));
        CHECK(!same_price(t.trail_alone.raw, t.adapter.raw));
    }
}

// ── 2. TradingView's tapes ─────────────────────────────────────────────

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::int64_t tape_ms(const std::string& text) {  // UTC+8 -> UTC ms
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

struct TapeTrade {
    std::int64_t entry_ms = 0, exit_ms = 0;
    double entry_price = kNaN, exit_price = kNaN;
    std::string exit_signal;
};

std::vector<TapeTrade> read_tape(const std::string& slug) {
    std::ifstream in(std::string(PINEFORGE_ZERO_TRAIL_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
    std::vector<TapeTrade> trades;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 5) continue;
        const std::size_t n = static_cast<std::size_t>(std::stoi(cell[0]));
        if (trades.size() < n) trades.resize(n);
        TapeTrade& t = trades[n - 1];
        const bool entry = cell[1].rfind("Entry", 0) == 0;
        (entry ? t.entry_ms : t.exit_ms) = tape_ms(cell[2]);
        (entry ? t.entry_price : t.exit_price) = std::stod(cell[4]);
        if (!entry) t.exit_signal = cell[3];
    }
    return trades;
}

void test_tapes() {
    struct Set { const char* slug; bool is_long; const double* points; const FeedBar* bars; int n; };
    const Set sets[] = {
        {"hm-orders-m16-f-long-zero-trail-open-on-best", true, kZeroTrailLongPoints,
         &kZeroTrailLong[0][0], 8},
        {"hm-orders-m16-f-short-zero-trail-open-on-best", false, kZeroTrailShortPoints,
         &kZeroTrailShort[0][0], 8},
    };
    for (const auto& set : sets) {
        std::printf("-- %s: %d TradingView trades --\n", set.slug, set.n);
        const auto tape = read_tape(set.slug);
        CHECK(static_cast<int>(tape.size()) == set.n);
        if (static_cast<int>(tape.size()) != set.n) continue;
        int adapter_matches = 0, trail_alone_matches = 0;
        for (int e = 0; e < set.n; ++e) {
            std::vector<Bar> bars;
            for (int i = 0; i < 28; ++i) {
                const FeedBar& fb = set.bars[e * 28 + i];
                Bar b{};
                b.timestamp = fb.ts; b.open = fb.open; b.high = fb.high;
                b.low = fb.low; b.close = fb.close; b.volume = 1000.0;
                bars.push_back(b);
            }
            const TapeTrade& tv = tape[static_cast<std::size_t>(e)];
            // The shape: the fill bar's close has reached the activation and
            // the next bar opens exactly on the carried best.
            CHECK(tv.entry_ms == bars[1].timestamp && same_price(tv.entry_price, bars[1].open));
            // TradingView exits on that next bar, within half a tick of its
            // open (the carried best's tick): the touch at the open.
            CHECK(tv.exit_ms == bars[2].timestamp);
            CHECK(std::fabs(tv.exit_price - bars[2].open) <= 0.005 + 1e-9);
            const Triple t = run_three(bars, set.is_long, set.points[e], 100.0);
            CHECK(t.adapter_has_sibling);
            CHECK(t.adapter.at_open && t.adapter.stop_leg && t.adapter.bar == 2);
            const bool adapter_ok = t.adapter_exit_bar == 2 && same_price(t.adapter_booked, tv.exit_price);
            const bool trail_ok = t.trail_alone.bar == 2 && same_price(t.trail_alone.raw, tv.exit_price);
            CHECK(adapter_ok);
            CHECK(!trail_ok);
            CHECK(same_bits(t.both_legs.raw, t.adapter.raw) && t.both_legs.at_open);
            adapter_matches += adapter_ok ? 1 : 0;
            trail_alone_matches += trail_ok ? 1 : 0;
            std::printf("        trade %d: TradingView @%.2f; adapter @%.10g (sibling Stop at the open); "
                        "Trail alone @%.10g (%s)\n", e + 1, tv.exit_price, t.adapter_booked,
                        t.trail_alone.raw, t.trail_alone.at_open ? "open" : "a later leg");
        }
        std::printf("        adapter %d/%d, kernel Trail alone %d/%d match TradingView\n",
                    adapter_matches, set.n, trail_alone_matches, set.n);
    }
}

}  // namespace

int main() {
    test_paired_differential();
    test_tapes();
    std::printf("\n%s zero-offset trail sibling stop: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
