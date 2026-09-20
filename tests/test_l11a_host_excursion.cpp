// R4-D L11a (RULING A48), the kernel half: per-lot excursion accounting is
// ONE generic capability of NativeStrategyHost, and a bare host gets the
// kernel's own model without declaring anything.
//
// Witnesses, all independent of the feature under test:
//   1. a bare host does not own the capability — owns_lot_excursions() is
//      false and closed_lot_excursion() answers zero magnitudes;
//   2. the kernel's own sampler: on a flat tape (O = H = L = C, so the only
//      delivered points are the closes) a lot opened at 100 that sees 103,
//      98 and 105 and closes at 101 reports favorable 10 and adverse 4 in
//      price-points x quantity — the running extremes, with the exit fill
//      itself folded in (max(carried, fill) on both sides);
//   3. a host that declares ownership supplies both magnitudes of the closing
//      row from the booking facts alone: the row carries the host's numbers
//      verbatim, every consultation carries the lot's own coordinates (entry
//      and fill price, lot and closed quantity, side, entry bar and time),
//      the settling consultation names the closing bar, and the carried
//      extremes are zero because the kernel keeps no excursion model of its
//      own for that run. The kernel builds the closing row for its account
//      projections as well as for the settlement itself, so the owner is
//      consulted more than once per lot; the count is not part of the
//      contract, the facts are.
//
// Source-free: this TU runs in the kernel-only profile. The source-host twin
// (the adapter declares ownership; ab9714be rows) is
// tests/test_l11a_host_excursion_twin.cpp.
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

NativeRunSpec spec(const char* key) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "TEST:L11A";
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

// Flat bars: every delivered point is a close, so the sampled path is exactly
// the close sequence below.
std::vector<Bar> tape() {
    const double closes[] = {100.0, 100.0, 103.0, 98.0, 105.0, 101.0, 100.0};
    std::vector<Bar> bars;
    int index = 0;
    for (double close : closes) {
        bars.push_back(mk(T + static_cast<std::int64_t>(index) * 60000, close, close, close, close));
        ++index;
    }
    return bars;
}

// Long 2 units at the close of bar 1 (AfterCalculation), flat at the close of
// bar 5: entry 100, exit 101, the lot rides 103 / 98 / 105 in between.
struct RoundTrip : NativeStrategyHost {
    int bars = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int index = bars++;
        if (index == 1) (void)submit({no::Transact{2.0}, "enter", ""});
        if (index == 5) (void)submit({no::Flatten{}, "exit", ""});
    }
};

// The same rule, owning its lots' excursion: the closing row must carry
// these magnitudes verbatim, and the facts must be the booking coordinates.
struct Owner final : RoundTrip {
    mutable std::vector<ClosedLotExcursionFacts> facts;
    bool owns_lot_excursions() const noexcept override { return true; }
    ClosedLotExcursion closed_lot_excursion(const ClosedLotExcursionFacts& f) const override {
        facts.push_back(f);
        return {7.25 * f.closed_qty, 3.5 * f.closed_qty};
    }
};

template <class HostType>
void run_round_trip(HostType& host, const char* key, const std::vector<Bar>& bars) {
    CHECK(host.configure_native(spec(key)).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.trade_count() == 1);
}

}  // namespace

int main() {
    const std::vector<Bar> bars = tape();
    {
        // 1. The bare host does not own the capability.
        RoundTrip bare;
        CHECK(!bare.owns_lot_excursions());
        ClosedLotExcursionFacts facts;
        facts.carried_favorable = 4.0;
        facts.carried_adverse = 2.0;
        facts.lot_qty = 1.0;
        facts.closed_qty = 1.0;
        const ClosedLotExcursion answer = bare.closed_lot_excursion(facts);
        CHECK(answer.favorable == 0.0);
        CHECK(answer.adverse == 0.0);
    }
    {
        // 2. The kernel's own model. Hand arithmetic, 2 units from 100:
        //   favorable = max((103-100)*2, (105-100)*2, (101-100)*2 at the fill) = 10
        //   adverse   = max((100-98)*2, -(101-100)*2 at the fill)               = 4
        RoundTrip host;
        run_round_trip(host, "l11a-kernel-model", bars);
        if (host.trade_count() == 1) {
            expect("kernel-model", host.get_trade(0), true, 100.0, 101.0, 10.0, 4.0);
        }
    }
    {
        // 3. The owning host: its numbers, its facts, nothing carried.
        Owner host;
        run_round_trip(host, "l11a-owner", bars);
        if (host.trade_count() == 1) {
            expect("owner", host.get_trade(0), true, 100.0, 101.0, 14.5, 7.0);
        }
        // Consulted for each projection of the closing row and for the
        // settlement: the same lot coordinates every time, nothing carried.
        CHECK(!host.facts.empty());
        bool settling_consultation = false;
        for (const ClosedLotExcursionFacts& f : host.facts) {
            CHECK(f.is_long);
            CHECK(near(f.entry_price, 100.0));
            CHECK(near(f.fill_price, 101.0));
            CHECK(near(f.lot_qty, 2.0));
            CHECK(near(f.closed_qty, 2.0));
            // The lot's own booking time: an AfterCalculation fill at the close
            // of bar 1 is stamped at that close, which is bar 2's open, and it
            // is the time the closed row reports as its entry.
            CHECK(f.entry_time_ms == bars[2].timestamp);
            CHECK(f.entry_time_ms == host.get_trade(0).entry_time);
            CHECK(f.entry_bar_index == 1);
            CHECK(f.carried_favorable == 0.0);
            CHECK(f.carried_adverse == 0.0);
            CHECK(!f.entry_bar_high_masked);
            CHECK(!f.entry_bar_low_masked);
            if (f.exit_bar_index == 5) settling_consultation = true;
        }
        CHECK(settling_consultation);
    }
    std::printf("test_l11a_host_excursion: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
