// Dormant setup and index allocation faults must preserve eager trades and hashes.
// Public stream/run/command calls fault real allocations, never sizes, request
// literals, or synthetic I/O; setup failures and index poisonings remain distinct.
#define PINEFORGE_DORMANT_BOOK_NO_MAIN
#include "test_native_dormant_book_transcript.cpp"
#include "global_allocation_replacement.hpp"

namespace allocation_test {
using namespace dormant_test;
constexpr std::size_t disarmed = std::numeric_limits<std::size_t>::max();

class Host final : public NativeStrategyHost {
public:
    Host(bool enabled, std::size_t fault) : fault_(fault) {
        set_broker_state_hash_recording(true);
        consumer().set_match_dormant(enabled);
    }
    Trace trace;
    bool armed = false;
    bool allocated_before_join = false;
    std::size_t allocations_before = 0;
    std::size_t refusals_before = 0;
    std::size_t applied = 0;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const std::vector<std::uint64_t>& recorded_hashes() const { return broker_state_hashes_; }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (++bar_ != 1) return;
        no::Request rest{no::Transact{1}, "allocation-rest", ""};
        rest.trigger = no::Limit{50};
        require(submit(rest).handle.has_value(), "allocation fault resting placement refused");
        no::Request fill{no::Transact{2}, "allocation-fill", ""};
        fill.trigger = no::Limit{99};
        require(submit(fill).handle.has_value(), "allocation fault filling placement refused");
    }
    void on_native_tick(const Bar&, const NativeTickContext&) override {
        if (armed) return;
        // A single warmup bar has placed rows but has not refreshed them at
        // an eligible point. No BookSpill exists and no row has joined yet.
        allocated_before_join = consumer().dormant_rows() == 0
            && !consumer().request_core().book_spilled();
        allocations_before = global_allocation::allocations;
        refusals_before = global_allocation::refused;
        armed = true;
        global_allocation::fail_after = fault_;
    }
    void on_native_applied(const no::ExecutionAppliedEvent& e,
                           const NativeDecisionContext&) override {
        require(e.request().label == "allocation-fill", "allocation fault run filled an unexpected request");
        // Hand oracle: the second tick is 98, through a buy limit of 99.
        // It opens the request's two units; the first tick at 100 cannot fill.
        require(e.resolved_price == 98 && e.opened_units == 2,
                "allocation fault fill changed the hand-derived price or units");
        ++applied;
        snapshot(*this, trace, 200);
    }
private:
    int bar_ = 0;
    std::size_t fault_;
};

struct Result {
    Trace trace;
    std::size_t allocations = 0;
    std::size_t refusals = 0;
    std::uint64_t setup = 0;
    std::uint64_t poison = 0;
    bool completed = false;
    bool threw = false;
};

void empty_book_digest() {
    namespace detail = pineforge::detail;
    // The real Book backing and the real run-store geometry. Equality is
    // the same store's own fold before and after its actual production
    // backing/cache is prepared. The comparator is an ordinary byte order.
    const detail::ScratchKeyOrder order{
        UINT64_C(0x5046424f4f4b0001), nullptr,
        +[](void*, const detail::ScratchByteSource& a, const detail::ScratchByteSource& b,
            int& result) noexcept {
            for (std::uint64_t i = 0; i < std::min(a.size(), b.size()); ++i) {
                std::byte left{}, right{};
                auto status = a.read(i, {&left, 1});
                if (status != detail::ScratchStatus::ok) return status;
                status = b.read(i, {&right, 1});
                if (status != detail::ScratchStatus::ok) return status;
                if (left != right) {
                    result = left < right ? -1 : 1;
                    return detail::ScratchStatus::ok;
                }
            }
            result = a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
            return detail::ScratchStatus::ok;
        }};
    DormantBookScratchBacking backing(512);
    detail::CommandScratchStore store(
        {2, 512, 1, 1, order, detail::ScratchOpenMode::create_empty}, &backing);
    require(store.status() == detail::ScratchStatus::ok && store.record_count() == 0,
            "empty book digest store failed to open");
    std::array<std::byte, 512> page{};
    std::uint64_t unprepared = 0, prepared = 0;
    require(store.state_digest({page.data(), page.size()}, unprepared)
                == detail::ScratchStatus::ok,
            "unprepared book state digest failed");
    const auto before = global_allocation::allocations;
    backing.prepare();
    require(global_allocation::allocations > before,
            "book digest control did not allocate its real backing");
    require(store.state_digest({page.data(), page.size()}, prepared)
                == detail::ScratchStatus::ok,
            "prepared book state digest failed");
    require(prepared == unprepared && store.record_count() == 0,
            "empty book preparation changed its state digest or row count");
}

Result run(bool enabled, std::size_t fault) {
    Host h(enabled, fault);
    auto spec = specification();
    spec.report_policy = NativeReportPolicy::HostRecorded;
    require(h.configure_native(spec).status == NativeSetupStatus::Applied,
            "allocation fault run configuration refused");
    const Bar warmup{100, 100, 100, 100, 1, start};
    require(h.stream_begin(&warmup, 1, "5", "5"), "allocation fault stream warmup failed");
    Result result;
    bool first = false;
    try {
        first = h.stream_push_tick({start + 300000, 1, 100, 1});
    } catch (const std::bad_alloc&) {
        result.threw = true;
    }
    // Disarm BEFORE assertions, result allocation, teardown or later ticks.
    global_allocation::fail_after = disarmed;
    require(h.armed && h.allocated_before_join,
            "allocation fault window was not armed before the first dormant join");
    result.allocations = global_allocation::allocations - h.allocations_before;
    result.refusals = global_allocation::refused - h.refusals_before;
    result.setup = h.consumer().dormant_setup_failures();
    result.poison = h.consumer().index_poisonings();
    const bool degraded = result.setup != 0 || result.poison != 0;
    if (fault == disarmed)
        require(!degraded, "healthy allocation control reported setup or index degradation");
    if (fault == disarmed || degraded) {
        require(first && !result.threw,
                "dormant allocation failure did not complete its matching tick");
        require(h.stream_push_tick({start + 300001, 2, 98, 1}),
                "allocation fault run failed its filling tick");
        require(h.stream_end(false), "allocation fault stream failed to complete");
        require(h.native_state().kind == NativeLifecycleKind::Completed
                    && h.native_state().failure.code == NativeFailureCode::None
                    && run_failure_value_of(h).code == RunFailureCode::none,
                "dormant allocation failure added a run failure code");
        require(h.applied == 1 && h.physical_position().signed_units == 2,
                "allocation fault run did not execute the two-unit fill");
        if (degraded) {
            require(h.consumer().eager_points() > 0,
                    "allocation degradation never used eager matching");
        } else if (enabled) {
            require(h.consumer().dormant_rows() > 0 && h.consumer().fast_points() > 0
                        && h.consumer().eager_points() == 0,
                    "healthy allocation control never used dormant matching");
        }
        finish(h, h.trace);
        result.trace = std::move(h.trace);
        result.completed = true;
    }
    return result;
}
} // namespace allocation_test

int main() {
    try {
        allocation_test::empty_book_digest();
        const auto eager = allocation_test::run(false, allocation_test::disarmed);
        const auto healthy = allocation_test::run(true, allocation_test::disarmed);
        dormant_test::require(eager.trace.words == healthy.trace.words,
                             "healthy allocation control changed eager output bytes");
        std::size_t setup_faults = 0, index_faults = 0;
        // Bound the fault-position sweep by this fixture's own healthy
        // allocation census, not an implementation size or a guessed cap.
        // Unrelated engine allocations may legitimately fail a run. Only
        // witnessed dormant setup/index degradation is required to succeed.
        for (std::size_t n = 0; n < healthy.allocations; ++n) {
            const auto faulted = allocation_test::run(true, n);
            dormant_test::require(faulted.refusals == 1,
                                 "allocation fault position did not refuse exactly one allocation");
            if (faulted.setup == 0 && faulted.poison == 0) continue;
            dormant_test::require(faulted.completed
                                     && faulted.trace.words == eager.trace.words,
                                 "allocation degradation changed eager trades or state hashes");
            if (faulted.setup != 0) {
                dormant_test::require(faulted.setup == 1 && faulted.poison == 0,
                                     "initial dormant setup failure was counted as index poison");
                ++setup_faults;
            } else ++index_faults;
        }
        dormant_test::require(setup_faults > 0,
                             "initial dormant setup allocation fault never degraded to eager");
        dormant_test::require(index_faults > 0,
                             "initial book backing allocation fault never poisoned the index");
        return 0;
    } catch (const std::exception& e) {
        global_allocation::fail_after = allocation_test::disarmed;
        std::fprintf(stderr, "test_native_dormant_book_allocation_fault: %s\n", e.what());
        return 1;
    }
}
