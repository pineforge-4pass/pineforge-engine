// A dormant book index write failure must disable fast matching and preserve
// eager output bytes. The production scratch observer records real I/O while
// a test backing budget faults the index without changing product limits.
#define PINEFORGE_DORMANT_BOOK_NO_MAIN
#include "test_native_dormant_book_transcript.cpp"
#include "../src/command_scratch_production.hpp"

namespace {
using namespace dormant_test;
namespace detail = pineforge::detail;
using Binding = NativeExecutionConsumer::CommandScratchBinding;
// Inspect the dormant book binding independently of dependency storage.
constexpr Binding book_binding = Binding::Book;

struct Charges {
    std::size_t errors = 0;
    std::size_t writes = 0;
    static void charge(void* context, const detail::ScratchIoCharge& c) noexcept {
        auto& self = *static_cast<Charges*>(context);
        if (c.operation == detail::ScratchIoOperation::write_page) ++self.writes;
        if (c.status == detail::ScratchStatus::io_error) ++self.errors;
    }
};

struct Registration {
    detail::CommandScratchByteCap previous_cap;
    detail::ScratchIoObserver previous_observer;
    Registration(std::uint64_t bytes, Charges& charges)
        : previous_cap(detail::set_command_scratch_byte_cap(bytes)),
          previous_observer(detail::set_command_scratch_io_observer({&charges, Charges::charge})) {}
    ~Registration() {
        detail::set_command_scratch_io_observer(previous_observer);
        detail::set_command_scratch_byte_cap(std::move(previous_cap));
    }
};

class FaultHost final : public NativeStrategyHost {
public:
    FaultHost(bool enabled, Charges& charges) : charges_(charges) {
        set_broker_state_hash_recording(true);
        consumer().set_match_dormant(enabled);
    }
    Trace trace;
    std::uint64_t fast_at_fault = 0;
    std::uint64_t fast_at_end = 0;
    std::uint64_t eager_at_fault = 0;
    std::size_t errors_at_fault = 0;
    bool saw_dormant = false;
    bool saw_spill = false;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const std::vector<std::uint64_t>& recorded_hashes() const { return broker_state_hashes_; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bar_;
        snapshot(*this, trace, 60);
        if (bar_ <= 2) {
            const auto first = bar_ == 1 ? 0 : resident;
            const auto last = bar_ == 1 ? resident : resident + 512;
            for (std::size_t i = first; i < last; ++i)
                require(submit(remote(i)).handle.has_value(), "index fault request placement refused");
        }
        saw_spill = saw_spill || consumer().request_core().book_spilled();
        saw_dormant = saw_dormant || consumer().dormant_rows() > 0;
        if (charges_.errors != 0 && errors_at_fault == 0) {
            errors_at_fault = charges_.errors;
            fast_at_fault = consumer().fast_points();
            eager_at_fault = consumer().eager_points();
        }
        fast_at_end = consumer().fast_points();
    }
private:
    Charges& charges_;
    int bar_ = 0;
};

Trace run(bool enabled, bool fault) {
    Charges charges;
    // create_empty writes no page (scratch_store_impl::initialize).
    // Independent transactions with no owner/group never write Dependency.
    // A Book index put needs a whole >=512-byte page; a ONE-BYTE shared
    // budget therefore faults its first write, after successful run setup.
    // This changes a test backing's failure condition, not any product cap.
    Registration registration(fault ? 1 : 0, charges);
    FaultHost h(enabled, charges);
    require(h.configure_native(specification()).status == NativeSetupStatus::Applied,
            "index fault run configuration refused");
    std::array<Bar, 7> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i)
        bars[i] = {100, 100, 100, 100, 1,
                   start + static_cast<std::int64_t>(i) * 300000};
    h.run(bars.data(), static_cast<int>(bars.size()));
    require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
    require(h.saw_spill && h.trade_count() == 0, "index fault run did not exercise a resting spilled book");
    const auto dependency = h.consumer().released_command_scratch(Binding::Dependency);
    require(dependency && dependency->status == detail::ScratchStatus::ok,
            "dependency snapshot missing or unhealthy during book index fault");
    require(h.consumer().released_command_scratch(book_binding).has_value(),
            "index fault run never provisioned the book binding");
    if (fault) {
        require(h.consumer().index_poisonings() > 0,
                "book index write failure did not count an index poisoning");
        require(h.consumer().dormant_setup_failures() == 0,
                "book index write failure was counted as a setup failure");
        require(charges.errors > 0 && h.errors_at_fault > 0,
                "book index write fault was not observed");
        require(charges.errors == h.errors_at_fault, "poisoned book index was retried");
        require(h.fast_at_end == h.fast_at_fault, "index poisoning did not keep fast matching disabled");
        require(h.consumer().eager_points() > h.eager_at_fault,
                "index poisoning did not continue through eager matching points");
    } else if (enabled) {
        require(h.consumer().index_poisonings() == 0
                    && h.consumer().dormant_setup_failures() == 0,
                "healthy book index reported a setup failure or poisoning");
        require(charges.errors == 0 && charges.writes > 0 && h.saw_dormant,
                "healthy index control did not exercise book writes and dormant rows");
        require(h.consumer().fast_points() > 0 && h.consumer().eager_points() == 0,
                "healthy index control did not use fast matching exclusively");
    }
    finish(h, h.trace);
    return std::move(h.trace);
}
} // namespace

int main() {
    try {
        const auto eager = run(false, false);
        const auto healthy = run(true, false);
        const auto faulted = run(true, true);
        require(eager.words == healthy.words, "healthy book index changed eager output bytes");
        require(eager.words == faulted.words, "book index write fault changed eager output bytes");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "test_native_dormant_book_index_fault: %s\n", e.what());
        return 1;
    }
}
