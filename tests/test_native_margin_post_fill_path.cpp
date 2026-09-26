// R5 lane PAR-MARGIN-2: the path a post-fill margin check measures.
//
// A resting request is matched at the driver point its segment reaches: a buy
// limit at 95 on a bar that runs O100 H101 L80 C81 is filled on the segment
// from the high to the low, and that fill is presented at the low's point. The
// book the fill leaves still faces the rest of that segment -- down to the low
// -- before the close. The kernel's AfterApplied point measured only the
// waypoints AFTER the point's phase, so it skipped the low and measured the
// close (the same held for a short filled on its way up to the high). It now
// measures from the segment's origin (NativeExecutionConsumer::
// margin_segment_origin), with the fill price as the point's own mark: the
// low is in the scan, the waypoints the path already passed are not.
// TradingView books the call at that low on the limit's own bar (lab tv tapes
// tests/fixtures/margin_entry_bar/pm2-m7-lim-*, pm2-m7-poocl-*,
// pm2-m7b-lim-*; the adapter half is test_adapter_margin_schedule_differential),
// as ab9714be's entry-bar suffix did.
//
// One margin model (maintenance only, the mark check, the kernel's own sizing),
// every check point admitted:
//   1. a long limit filled on the H->L leg of a high-first bar: the post-fill
//      point measures 80 (it measured 81, the close) and the call rests and
//      books at 80;
//   2. a long limit filled on the O->L leg of a low-first bar: 80 breaches
//      where the high and the close after it do not -- the call is booked on
//      the bar (it was booked nowhere);
//   3. a short limit filled on the L->H leg of a low-first bar: 120 (it
//      measured the fill price, 105, and booked nothing);
//   4. a market fill at the open is unchanged: its remaining path already
//      started at the open.
//
// Source-free: runs in the kernel-only profile too.
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int checks = 0;
int failures = 0;
#define CHECK(expr) do {                                                       \
    ++checks;                                                                  \
    if (!(expr)) { ++failures; std::printf("FAIL %d %s\n", __LINE__, #expr); } \
} while (0)

struct Point {
    NativeMarginCheckKind kind;
    int bar;
    double mark;
};

struct Probe final : NativeStrategyHost {
    no::Request order;
    int bars = 0;
    mutable std::vector<Point> points;
    std::vector<no::MarginCallEvent> calls;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (bars++ == 0) (void)submit(order);
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& p) const override {
        points.push_back({p.kind, p.cursor.point.interval_index, p.mark});
        return true;
    }
    void on_native_margin_call(const no::MarginCallEvent& e) override { calls.push_back(e); }
    // The AfterApplied point of the bar the order filled on.
    const Point* after_applied(int bar) const {
        for (const auto& p : points) {
            if (p.kind == NativeMarginCheckKind::AfterApplied && p.bar == bar) return &p;
        }
        return nullptr;
    }
};

const int64_t T0 = 1791190800000LL;   // Mon 5 Oct 2026 09:00 UTC
const int64_t M15 = 15 * 60000LL;

NativeRunSpec spec() {
    NativeRunSpec s;
    s.identity = {"margin-post-fill-path", 1};
    s.input_tf = "15";
    s.script_tf = "15";
    s.ticker = "P";
    s.tickerid = "PROBE:P";
    s.type = "cfd";
    s.currency = "USD";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 1000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.event_retention = NativeEventRetention::Full;
    NativeMarginModel m;
    m.initial_long = 0.0;             // maintenance only: the host admits
    m.initial_short = 0.0;
    m.maintenance_long = 0.5;
    m.maintenance_short = 0.5;
    m.sizing = NativeLiquidationSizing::RestoreMinimum;
    m.check = NativeLiquidationCheck::PathAdverseExtremeMark;
    s.margin = m;
    return s;
}

no::Request limit(double units, double level) {
    no::Request r;
    r.intent = no::Transact{units};
    r.label = "entry";
    r.trigger = no::Limit{level};
    return r;
}

void run(Probe& h, const std::vector<Bar>& bars) {
    const auto setup = h.configure_native(spec());
    CHECK(setup.status == NativeSetupStatus::Applied);
    if (setup.status != NativeSetupStatus::Applied) return;
    h.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(h.last_error().empty());
}

void print(const char* label, const Probe& h, int bar) {
    const Point* p = h.after_applied(bar);
    std::printf("%-44s AfterApplied mark %.6g  calls %zu", label, p ? p->mark : -1.0,
                h.calls.size());
    for (const auto& c : h.calls)
        std::printf("  [bar %d units %.6g at %.4f]", c.cursor.point.interval_index, c.units, c.mark);
    std::printf("  position %.6g\n", h.physical_position().signed_units);
}

std::vector<Bar> tape(std::initializer_list<std::vector<double>> rows) {
    std::vector<Bar> out;
    int64_t t = T0;
    for (const auto& r : rows) {
        out.push_back({r[0], r[1], r[2], r[3], 10, t});
        t += M15;
    }
    return out;
}

// 1. High-first bar: O100 H101 L80 C81, a buy limit of 20 at 95 filled on the
// way down. At 80 the book is 700 against 800; at the close, 720 against 810.
void long_limit_high_first() {
    Probe h;
    h.order = limit(20.0, 95.0);
    run(h, tape({{100, 100.5, 99.5, 100}, {100, 101, 80, 81}, {100, 100.5, 99.5, 100}}));
    print("long limit on the H->L leg (high first)", h, 1);
    const Point* p = h.after_applied(1);
    CHECK(p != nullptr);
    if (p) CHECK(p->mark == 80.0);
    CHECK(!h.calls.empty());
    if (!h.calls.empty()) {
        CHECK(h.calls[0].cursor.point.interval_index == 1);
        CHECK(h.calls[0].mark == 80.0);
    }
}

// 2. Low-first bar: O100 L80 H121 C100 (|O-L| 20 < |H-O| 21), the same limit
// filled on the way down to the low. At 80: 700 against 800. The high and the
// close after it are solvent (at 95 the book is 1000 against 950).
void long_limit_low_first() {
    Probe h;
    h.order = limit(20.0, 95.0);
    run(h, tape({{100, 100.5, 99.5, 100}, {100, 121, 80, 100}, {100, 100.5, 99.5, 100}}));
    print("long limit on the O->L leg (low first)", h, 1);
    const Point* p = h.after_applied(1);
    CHECK(p != nullptr);
    if (p) CHECK(p->mark == 80.0);
    CHECK(!h.calls.empty());
    if (!h.calls.empty()) {
        CHECK(h.calls[0].cursor.point.interval_index == 1);
        CHECK(h.calls[0].mark == 80.0);
    }
}

// 3. A sell limit of 15 at 105 on a low-first bar O100 L99 H120 C101: filled
// on the way up to the high. At 120 the short's book is 775 against 900; at
// the fill, 1000 against 787.5.
void short_limit_rising_leg() {
    Probe h;
    h.order = limit(-15.0, 105.0);
    run(h, tape({{100, 100.5, 99.5, 100}, {100, 120, 99, 101}, {100, 100.5, 99.5, 100}}));
    print("short limit on the L->H leg (low first)", h, 1);
    const Point* p = h.after_applied(1);
    CHECK(p != nullptr);
    if (p) CHECK(p->mark == 120.0);
    CHECK(!h.calls.empty());
    if (!h.calls.empty()) {
        CHECK(h.calls[0].cursor.point.interval_index == 1);
        CHECK(h.calls[0].mark == 120.0);
    }
}

// 4. A market buy of 20 at bar 1's open (O100 H101 L80 C81): the point is the
// open's own, whose remaining path already started there -- the low, as before.
void market_at_open_unchanged() {
    Probe h;
    no::Request r;
    r.intent = no::Transact{20.0};
    r.label = "entry";
    h.order = r;
    run(h, tape({{100, 100.5, 99.5, 100}, {100, 101, 80, 81}, {100, 100.5, 99.5, 100}}));
    print("market at the open (control)", h, 1);
    const Point* p = h.after_applied(1);
    CHECK(p != nullptr);
    if (p) CHECK(p->mark == 80.0);
    CHECK(!h.calls.empty());
    if (!h.calls.empty()) CHECK(h.calls[0].mark == 80.0);
}

}  // namespace

int main() {
    long_limit_high_first();
    long_limit_low_first();
    short_limit_rising_leg();
    market_at_open_unchanged();
    std::printf("test_native_margin_post_fill_path: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
