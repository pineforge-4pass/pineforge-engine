// R5 lane E6 (rule 5's last adapter-written kernel pair): the entry-bar
// excursion mask is DECLARED through a generic kernel capability, not poked
// into a lot by a host that reached into `PyramidEntry`.
//
// The mask says which end of a lot's ENTRY bar the delivered path had already
// reached before that lot's own opening fill, so that end is not part of the
// lot's excursion. It is the excursion owner's statement (RULING A48,
// owns_lot_excursions), and the only thing the owner has to say is WHERE on
// the bar its fill sits: on the path, or after the whole of it. The geometry
// — which leg the path walks first and where a price is first touched — is
// the kernel's own (`bar_path_uses_high_first`, `first_touch_position`), and
// it is the kernel that derives the two flags from it.
//
// Witnesses, source-free (this TU runs in the kernel-only profile):
//   1. a host that declares nothing leaves both flags clear, and the closing
//      row carries the whole entry bar;
//   2. OnPath over a high-first bar (|H-O| < |O-L|) masks the HIGH and only
//      the high: the path is O->H->L->C and a fill at the close is reached
//      after the high, before the low;
//   3. OnPath over a low-first bar masks the LOW and only the low — the same
//      declaration, the opposite answer, because the geometry is the bar's;
//   4. AfterPath masks both ends, the statement a fill at the bar's close
//      point makes;
//   5. the declaration is scoped to one entry incarnation: declaring another
//      incarnation's mask leaves this lot's flags clear;
//   6. and the mask DRIVES THE SKIP — the owner's magnitudes, and therefore
//      the closed row's, differ by exactly the masked end.
#include "l11a_host_excursion_fixture.hpp"

#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {
using namespace l11a_fixture;

constexpr std::int64_t T = 1736121600000LL;
constexpr double kQty = 2.0;

NativeRunSpec spec(const char* key) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "TEST:E6";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    s.close_execution = NativeCloseExecution::AfterCalculation;
    return s;
}

// What the host declares about its lot's fill, if anything.
enum class Declare { Nothing, OnPath, AfterPath, OtherIncarnation };

// Bar 1 is the entry bar: an AfterCalculation market entry fills at its
// close, so the lot's own price is that close and the path position of the
// fill is a property of this bar alone.
std::vector<Bar> tape(double h1, double l1, double c1) {
    std::vector<Bar> bars;
    bars.push_back(mk(T, 100.0, 100.0, 100.0, 100.0));
    bars.push_back(mk(T + 60000, 100.0, h1, l1, c1));
    bars.push_back(mk(T + 120000, c1, c1, c1, c1));
    bars.push_back(mk(T + 180000, c1, c1, c1, c1));
    bars.push_back(mk(T + 240000, c1, c1, c1, c1));
    bars.push_back(mk(T + 300000, c1, c1, c1, c1));
    return bars;
}

// Enter long at the close of bar 1, flat at the close of bar 4. The mask is
// declared from on_native_applied, exactly where a host learns that a lot was
// opened and at which cursor — the seam the Pine adapter uses.
struct MaskDeclaringHost : NativeStrategyHost {
    Declare mode = Declare::Nothing;
    Bar current{};
    int bars = 0;
    int declarations = 0;

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        current = bar;
        const int index = bars++;
        if (index == 1) (void)submit({no::Transact{kQty}, "enter", ""});
        if (index == 4) (void)submit({no::Flatten{}, "exit", ""});
    }

    void on_native_applied(const native_order::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        if (event.opened_units == 0.0) return;
        const std::uint64_t incarnation = event.handle().incarnation;
        ++declarations;
        switch (mode) {
        case Declare::Nothing:
            break;
        case Declare::OnPath:
            declare_opened_lot_entry_bar_mask(incarnation, current,
                                              OpenedLotFillPoint::OnPath);
            break;
        case Declare::AfterPath:
            declare_opened_lot_entry_bar_mask(incarnation, current,
                                              OpenedLotFillPoint::AfterPath);
            break;
        case Declare::OtherIncarnation:
            declare_opened_lot_entry_bar_mask(incarnation + 1, current,
                                              OpenedLotFillPoint::AfterPath);
            break;
        }
    }
};

// The owner reads the mask and skips the masked end, which is what an owner
// of the excursion does with it. Nothing else of the bar crosses the seam:
// the two extremes are the owner's own record of the entry bar.
struct Owner final : MaskDeclaringHost {
    double entry_bar_high = 0.0;
    double entry_bar_low = 0.0;
    mutable std::vector<ClosedLotExcursionFacts> facts;

    bool owns_lot_excursions() const noexcept override { return true; }
    ClosedLotExcursion closed_lot_excursion(const ClosedLotExcursionFacts& f) const override {
        facts.push_back(f);
        const double high = f.entry_bar_high_masked ? f.entry_price : entry_bar_high;
        const double low = f.entry_bar_low_masked ? f.entry_price : entry_bar_low;
        return {(high - f.entry_price) * f.closed_qty,
                (f.entry_price - low) * f.closed_qty};
    }
};

struct Case {
    const char* tag;
    Declare mode;
    double high;
    double low;
    double close;
    bool want_high_masked;
    bool want_low_masked;
    double want_favorable;
    double want_adverse;
};

void run(const Case& c) {
    const std::vector<Bar> bars = tape(c.high, c.low, c.close);
    Owner host;
    host.mode = c.mode;
    host.entry_bar_high = c.high;
    host.entry_bar_low = c.low;
    CHECK(host.configure_native(spec(c.tag)).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.declarations > 0);
    CHECK(host.trade_count() == 1);
    if (host.trade_count() != 1) return;

    CHECK(!host.facts.empty());
    for (const ClosedLotExcursionFacts& f : host.facts) {
        CHECK(near(f.entry_price, c.close));
        CHECK(near(f.closed_qty, kQty));
        CHECK(f.entry_bar_high_masked == c.want_high_masked);
        CHECK(f.entry_bar_low_masked == c.want_low_masked);
    }
    expect(c.tag, host.get_trade(0), true, c.close, c.close,
           c.want_favorable, c.want_adverse);
}

}  // namespace

int main() {
    // A high-first entry bar: |H - O| = 1 < |O - L| = 10, so the modeled path
    // is O(100) -> H(101) -> L(90) -> C(95) and the fill at 95 is first
    // touched on the H->L leg, after the high and before the low.
    const Case high_first[] = {
        {"nothing-high-first", Declare::Nothing, 101.0, 90.0, 95.0,
         false, false, (101.0 - 95.0) * kQty, (95.0 - 90.0) * kQty},
        {"onpath-high-first", Declare::OnPath, 101.0, 90.0, 95.0,
         true, false, 0.0, (95.0 - 90.0) * kQty},
        {"afterpath-high-first", Declare::AfterPath, 101.0, 90.0, 95.0,
         true, true, 0.0, 0.0},
        {"other-incarnation", Declare::OtherIncarnation, 101.0, 90.0, 95.0,
         false, false, (101.0 - 95.0) * kQty, (95.0 - 90.0) * kQty},
    };
    for (const Case& c : high_first) run(c);

    // The same declaration over a low-first bar: |H - O| = 10 is not less
    // than |O - L| = 1, the path is O(100) -> L(99) -> H(110) -> C(105), and
    // the fill at 105 is reached after the low and before the high.
    const Case low_first[] = {
        {"nothing-low-first", Declare::Nothing, 110.0, 99.0, 105.0,
         false, false, (110.0 - 105.0) * kQty, (105.0 - 99.0) * kQty},
        {"onpath-low-first", Declare::OnPath, 110.0, 99.0, 105.0,
         false, true, (110.0 - 105.0) * kQty, 0.0},
        {"afterpath-low-first", Declare::AfterPath, 110.0, 99.0, 105.0,
         true, true, 0.0, 0.0},
    };
    for (const Case& c : low_first) run(c);

    std::printf("test_e6_entry_bar_mask_declaration: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
