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

// M14 needs a bound close BEFORE the spill, and a callback before a stale
// close can repair itself as an Evaluate winner. All entries share one limit:
// their wake order is priority order, with the spilled entry waking last.
// The signal fills only half its units, leaving no resident vacancy for the
// pre-notification bound-expiry drain to promote the spilled close into.
// Fixture sizes and behavior are layout estimates from source, not measured.
class SpilledCloseHost final : public NativeStrategyHost {
public:
    SpilledCloseHost(bool prepared, bool enabled) {
        set_broker_state_hash_recording(true);
        consumer().set_direct_mutation(!prepared);
        consumer().set_match_dormant(enabled);
    }
    Trace trace;
    bool saw_signal = false;
    std::size_t spilled_book_closes = 0;
    std::uint64_t dormant_rows_at_comparison = 0;
    std::uint64_t fast_points_at_comparison = 0;
    std::uint64_t wakes_at_comparison = 0;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const std::vector<std::uint64_t>& recorded_hashes() const { return broker_state_hashes_; }

    void on_native_run_begin() override {
        put(no::Request{no::Transact{2}, "spilled-close-open", ""});
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (++bar_ == 1) {
            for (std::size_t i = 0; i < resident - 1; ++i) {
                no::Request entry{no::Transact{i == 0 ? 2.0 : 1.0},
                    i == 0 ? "spilled-close-signal" : "spilled-close-entry", ""};
                entry.trigger = no::Limit{99};
                // min(remaining 2, budget 1) fills 1 and keeps 1 live.
                if (i == 0) entry.capacity = no::PointBudget{1};
                entries_.push_back(put(entry));
            }
            closes_[0] = put(close_request("spilled-close-first"));
        } else if (bar_ == 2) {
            const auto& core = consumer().request_core();
            const auto first = core.hold_live(closes_[0]);
            require(first && std::holds_alternative<no::BookClose>(first->authority),
                    "T4 spilled-close first close was not bound before spilling");
            // Eager spills the bound close; dormant spills the last entry.
            // The quiet third bar binds the new tail close without filling.
            closes_[1] = put(close_request("spilled-close-tail"));
            require(core.live_size() > resident && core.book_spilled(),
                    "T4 spilled-close population did not exceed kBookMax");
        }
    }
    void on_native_applied(const no::ExecutionAppliedEvent& e,
                           const NativeDecisionContext&) override {
        if (e.request().label != "spilled-close-signal") return;
        require(!saw_signal && e.cursor.t == 0 && e.resolved_price == 98,
                "T4 spilled-close signal missed the opening gap");
        require(physical_position().signed_units == 3,
                "T4 spilled-close signal did not add one unit to two");
        const auto& core = consumer().request_core();
        const auto* remaining = std::get_if<no::RemainingProjectionUnits>(&e.remaining_after);
        require(!e.terminal && remaining && remaining->q == 1
                    && core.live_size() == resident + 1,
                "T4 spilled-close partial signal left a resident vacancy");
        std::array<no::Allowance, 2> allowances{};
        for (std::size_t i = 0; i < closes_.size(); ++i) {
            const auto reads = core.book_lookup_reads().rows;
            const auto held = core.hold_live(closes_[i]);
            require(held && std::holds_alternative<no::BookClose>(held->authority),
                    "T4 spilled-close comparison lost a bound close");
            const auto& bound = std::get<no::BookClose>(held->authority);
            require(bound.cycle == e.cycle_after && bound.side == no::Side::Long,
                    "T4 spilled-close comparison bound to wrong cycle/side");
            if (core.book_lookup_reads().rows > reads) ++spilled_book_closes;
            const auto* units = std::get_if<no::AllowanceUnits>(&held->allowance);
            // Independent arithmetic: min(remaining 1, point budget 1/4) = 1/4.
            // Neither unreachable sell limit can spend any of that budget.
            require(units && units->initial == 0.25 && units->left == 0.25,
                    "T4 spilled-close allowance quantity differs from hand arithmetic");
            allowances[i] = held->allowance;
        }
        dormant_rows_at_comparison = consumer().dormant_rows();
        fast_points_at_comparison = consumer().fast_points();
        wakes_at_comparison = consumer().wakes_by_price();
        // Before any cancellation, both runs still have a spilled BookClose.
        // With M14 eager has one old point Q < P; dormant bulk-refreshed both
        // resident closes at P before the entry wake spilled its tail close.
        // Bound-expiry checking can swap the two closes without refreshing
        // allowances; the partial signal keeps the resident book full.
        trace.add(160, e.cursor.point.ordinal);
        trace.add(161, std::min(std::get<no::AllowanceUnits>(allowances[0]).point_ordinal,
                                std::get<no::AllowanceUnits>(allowances[1]).point_ordinal));
        record_allowance(trace, 162, allowances[0]);
        record_allowance(trace, 166, allowances[1]);
        trace.add(170, native_continuation_hash());
        trace.add(171, broker_state_hash());
        saw_signal = true;
        // Observe first, then cancel every entry, including the signal's
        // remaining unit, so this row needs only the one partial gap fill.
        for (std::size_t i = 0; i < entries_.size(); ++i)
            require(cancel(entries_[i]).status == no::CancelStatus::Cancelled,
                    "T4 spilled-close entry cancellation refused");
    }

private:
    static no::Request close_request(const char* label) {
        no::Request close{no::Reduce{no::ExplicitUnits{1}}, label, ""};
        close.trigger = no::Limit{1000};
        close.capacity = no::PointBudget{0.25};
        return close;
    }
    no::RequestHandle put(const no::Request& request) {
        const auto result = submit(request);
        require(result.status == no::SubmitStatus::Accepted && result.handle.has_value(),
                "T4 spilled-close submission refused");
        return *result.handle;
    }
    int bar_ = 0;
    std::vector<no::RequestHandle> entries_;
    std::array<no::RequestHandle, 2> closes_{};
};

Trace switched_spilled_close(bool prepared, bool enabled) {
    SpilledCloseHost h(prepared, enabled);
    require(h.configure_native(specification()).status == NativeSetupStatus::Applied,
            "T4 spilled-close configure refused");
    const std::array<Bar, 4> bars{{
        {100, 101, 100, 100, 1, start},
        {100, 101, 100, 100, 1, start + 300000},
        {100, 101, 100, 100, 1, start + 600000},
        {98, 100, 98, 98, 1, start + 900000}
    }};
    h.run(bars.data(), static_cast<int>(bars.size()));
    require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
    require(h.saw_signal, "T4 spilled-close comparison callback did not run");
    require(h.spilled_book_closes > 0,
            "T4 spilled-close comparison had no spilled BookClose");
    if (enabled) {
        require(h.dormant_rows_at_comparison > 0, "T4 spilled-close on-run had no dormant rows");
        require(h.fast_points_at_comparison > 0, "T4 spilled-close on-run took no fast point");
        require(h.wakes_at_comparison > 0, "T4 spilled-close on-run did no price wake");
    } else {
        require(h.dormant_rows_at_comparison == 0, "T4 spilled-close off-run joined dormant rows");
        require(h.fast_points_at_comparison == 0, "T4 spilled-close off-run took a fast point");
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
            const auto close_off = switched_spilled_close(prepared, false);
            const auto close_on = switched_spilled_close(prepared, true);
            require(close_off.words == close_on.words,
                    "T4 spilled-close eager/dormant bytes differ");
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
