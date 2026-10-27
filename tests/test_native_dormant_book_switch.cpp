// C3 / T4. Switch contract: DESIGN sections 4.7 and 18,
// void NativeExecutionConsumer::set_match_dormant(bool), quiet_points-style
// uint64 diagnostics. No on/off choice is part of the transcript or hash.
#define PINEFORGE_DORMANT_BOOK_NO_MAIN
#include "test_native_dormant_book_transcript.cpp"

namespace {
using namespace dormant_test;

Trace switched(bool enabled, bool prepared) {
    Host h(0x12345678, prepared, enabled);
    // Expected first base diagnostic: NativeExecutionConsumer has no member
    // named set_match_dormant. Keep this unconditional.
    h.consumer().set_match_dormant(enabled);
    require(h.configure_native(specification()).status == NativeSetupStatus::Applied,
            "T4 configure refused");
    const auto bars = tape();
    h.run(bars.data(), static_cast<int>(bars.size()));
    require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
    require(h.crossed_up && h.crossed_down && h.ring_before && h.ring_after,
            "T4 boundary/callback seed did not run");
    if (enabled) {
        require(h.consumer().dormant_rows() > 0, "T4 on-run had no dormant rows");
        require(h.consumer().fast_points() > 0, "T4 on-run took no fast point");
        require(h.consumer().projected_reads() > 0, "T4 on-run did no projection");
        require(h.consumer().wakes_by_price() > 0, "T4 on-run did no price wake");
        require(h.consumer().eager_points() == 0, "T4 on-run unexpectedly fell back");
    } else {
        require(h.consumer().dormant_rows() == 0, "T4 off-run joined dormant rows");
        require(h.consumer().fast_points() == 0, "T4 off-run took a fast point");
        require(h.consumer().eager_points() > 0, "T4 off-run matched no eager point");
    }
    finish(h, h.trace);
    return std::move(h.trace);
}
} // namespace

int main() {
    try {
        for (const bool prepared : {false, true}) {
            const auto off = switched(false, prepared);
            const auto on = switched(true, prepared);
            require(off.words == on.words, "T4 eager/dormant transcript bytes differ");
            const auto active_off = dormant_test::run_active(prepared, false);
            const auto active_on = dormant_test::run_active(prepared, true);
            require(active_off.words == active_on.words,
                    "T4 active-spill eager/dormant bytes differ");
            const auto overflow_off = dormant_test::run_overflow(prepared, false);
            const auto overflow_on = dormant_test::run_overflow(prepared, true);
            require(overflow_off.words == overflow_on.words,
                    "T4 mid-point active-spill eager/dormant bytes differ");
            const auto ring_off = dormant_test::run_ring_window(prepared, false);
            const auto ring_on = dormant_test::run_ring_window(prepared, true);
            require(ring_off.words == ring_on.words, "T4 keepalive eager/dormant bytes differ");
            const auto middle_off = dormant_test::run_middle_spill(prepared, false);
            const auto middle_on = dormant_test::run_middle_spill(prepared, true);
            require(middle_off.words == middle_on.words, "T4 middle-spill eager/dormant bytes differ");
            const auto drivers_off = dormant_test::run_driver_matrix(prepared, false);
            const auto drivers_on = dormant_test::run_driver_matrix(prepared, true);
            require(drivers_off.words == drivers_on.words, "T4 driver-matrix eager/dormant bytes differ");
            const auto complete_off = dormant_test::run_complete(prepared, false);
            const auto complete_on = dormant_test::run_complete(prepared, true);
            require(complete_off.words == complete_on.words,
                    "T4 complete-before-interior eager/dormant bytes differ");
            const auto trail_off = dormant_test::run_trail(prepared, false);
            const auto trail_on = dormant_test::run_trail(prepared, true);
            require(trail_off.words == trail_on.words, "T4 active-trail eager/dormant bytes differ");
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "test_native_dormant_book_switch: %s\n", e.what());
        return 1;
    }
}
