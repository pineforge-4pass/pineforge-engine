// B1-06: an accepted event larger than the ordinary spill buffer must survive
// spilling. On the base, admission succeeds and a later unrelated submit throws
// ScratchIoError(EFBIG) when the first event finally reaches the spill front.
#include <pineforge/native_order.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
namespace no = pineforge::native_order;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void large_event_spill() {
    no::WorkingRequestCore core(no::RunIdentity{"p2-b1-06-large-event", 1});
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    // The comment alone exceeds JournalStore::kSpillPageBytes; event framing
    // and the definition make the serialized record larger still.
    constexpr std::size_t kSpillPageBytes = 32768;
    constexpr std::size_t kResidentEvents = 2048;
    const std::string comment(2 * kSpillPageBytes + 1, 'x');
    const no::Request large{no::Transact{1.0}, "large", comment};
    const auto accepted = core.submit(large, 0, incarnation, ordinal);
    require(accepted.status == no::SubmitStatus::Accepted && accepted.handle,
            "large event was not accepted at its submitting command");
    const no::EventId accepted_id{core.identity(), accepted.event_ordinal};

    // A rejected request carries its attempted payload too. Exercise both
    // event alternatives and multiple full buffer flushes plus a partial tail.
    const auto rejected = core.submit(
        no::Request{no::Transact{0.0}, "large-rejected", comment}, 0, incarnation, ordinal);
    require(rejected.status == no::SubmitStatus::Rejected
                && rejected.reason == no::RequestRejectReason::InvalidQuantity,
            "large rejected event changed admission behavior");
    const no::EventId rejected_id{core.identity(), rejected.event_ordinal};

    for (std::size_t i = 0; i != kResidentEvents + 32; ++i) {
        require(core.submit(no::Request{no::Transact{0.0}, "advance", ""},
                            0, incarnation, ordinal).status == no::SubmitStatus::Rejected,
                "unrelated command failed after the large event was accepted");
    }
    require(core.history_end() > kResidentEvents + 2,
            "test did not exceed the resident journal bound");
    {
        const auto* event = core.event_at(accepted_id);
        const auto* row = event ? std::get_if<no::AcceptedEvent>(event) : nullptr;
        require(row && row->definition, "large accepted event was lost during spill");
        require(row->handle() == *accepted.handle, "large event handle changed");
        require(row->request().label == "large" && row->request().comment == comment,
                "large accepted payload did not round-trip");
    }
    {
        const auto* event = core.event_at(rejected_id);
        const auto* row = event ? std::get_if<no::RejectedEvent>(event) : nullptr;
        require(row && row->request.label == "large-rejected"
                    && row->request.comment == comment,
                "large rejected payload did not round-trip");
        require(row->reason == no::RequestRejectReason::InvalidQuantity,
                "large rejected event reason changed");
    }
    require(std::holds_alternative<no::RejectedEvent>(core.history_at(2)),
            "ordinary page after oversized pages did not round-trip");
}
} // namespace

int main() {
    try {
        large_event_spill();
        std::puts("test_journal_large_event_spill: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_journal_large_event_spill: FAIL: %s\n", error.what());
        return 1;
    }
}
