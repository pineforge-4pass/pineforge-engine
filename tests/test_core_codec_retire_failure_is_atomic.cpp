// 1.7.0 review B1-03, the retirement half: a journal retirement that fails in
// the bound codec store changes nothing.
//
// With a journal codec bound (bind_journal_codec), retiring a prefix erases its
// records from the borrowed CommandScratchStore inside one store transaction;
// a commit that fails rolls the store back, and the erase throws.
// WorkingRequestCore::retire_history catches that and answers 0, but it had
// already moved retired_through_ to the newest event it meant to retire: the
// window [history_base, history_end) stayed as it was while retired_through()
// claimed a retirement that never happened (a retained receipt outcome is
// then no longer re-checked, and a valid receipt is refused).
//
// The run: a core bound to a codec store over a backing whose page writes can
// be made to fail; eight submits (one event each); a first retirement of the
// oldest three with the backing healthy (the mechanism works: the window and
// retired_through move); then page writes fail and a second retirement of
// three more is attempted. It must reach the fault, answer 0, and leave
// retired_through, history_base, history_end and the live rows as they were.
// Only writes fail: every read retire_history makes before it changes
// anything (the ordinal search) still succeeds, so the failure is the erase's.
//
// Previous-epoch API only (bind_journal_codec is declared in native_order.hpp
// at the base), so this TU compiles against the unfixed head and fails there
// on retired_through.
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/native_order.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <variant>
#include <vector>

namespace {

using namespace pineforge::detail;
namespace no = pineforge::native_order;

int failures = 0;
int checks = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            ++failures;                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
        }                                                                           \
    } while (false)
#define REQUIRE(x)                                                                  \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            ++failures;                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
            throw std::runtime_error(#x);                                           \
        }                                                                           \
    } while (false)

class FaultBacking final : public ScratchBacking {
public:
    explicit FaultBacking(FixedMemoryScratchBacking& inner) noexcept
        : ScratchBacking(inner.kind(), inner.page_size()), inner_(inner) {}

    std::uint64_t page_count() const noexcept override { return inner_.page_count(); }

    bool fail_writes = false;
    std::uint64_t write_faults = 0;

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override {
        return inner_.read_page(id, destination);
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override {
        if (fail_writes) {
            ++write_faults;
            return ScratchStatus::io_error;
        }
        return inner_.write_page(id, source);
    }
    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        return inner_.sync(durability);
    }

private:
    FixedMemoryScratchBacking& inner_;
};

// The journal codec's keys are 8-byte little-endian positions.
ScratchStatus numeric_order(void*, const ScratchByteSource& a, const ScratchByteSource& b,
                            int& order) noexcept {
    if (a.size() != 8 || b.size() != 8) return ScratchStatus::programming_error;
    std::array<std::byte, 8> left{}, right{};
    auto status = a.read(0, {left.data(), left.size()});
    if (status != ScratchStatus::ok) return status;
    status = b.read(0, {right.data(), right.size()});
    if (status != ScratchStatus::ok) return status;
    order = 0;
    for (std::size_t i = 8; i-- != 0;) {
        if (left[i] != right[i]) {
            order = std::to_integer<unsigned>(left[i]) < std::to_integer<unsigned>(right[i]) ? -1 : 1;
            break;
        }
    }
    return ScratchStatus::ok;
}

std::uint64_t ordinal_at(const no::WorkingRequestCore& core, std::size_t absolute) {
    const auto* accepted = std::get_if<no::AcceptedEvent>(&core.history_at(absolute));
    REQUIRE(accepted != nullptr);
    return accepted->ordinal;
}

void failed_codec_retirement_changes_nothing() {
    constexpr std::size_t kPage = 512;
    std::vector<std::byte> storage(1024 * kPage);
    FixedMemoryScratchBacking memory({storage.data(), storage.size()}, kPage);
    FaultBacking fault(memory);
    CommandScratchStore store{
        {2, kPage, 1, 1, {0x62312d3033726574ULL, nullptr, &numeric_order},
         ScratchOpenMode::create_empty},
        &fault};
    REQUIRE(store.status() == ScratchStatus::ok);

    no::RunIdentity run{"b1-03-codec-retire", 1};
    no::WorkingRequestCore core(run);
    no::bind_journal_codec(core, store);
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    no::Request request{no::Transact{1.0}, "rest", ""};
    request.trigger = no::Limit{50.0};
    for (int i = 0; i != 8; ++i) {
        REQUIRE(core.submit(request, 0, incarnation, ordinal).status == no::SubmitStatus::Accepted);
    }
    REQUIRE(core.history_end() == 8);

    // Healthy: the oldest three retire, and retired_through names the newest
    // of them.
    const std::uint64_t third = ordinal_at(core, 2);
    REQUIRE(core.retire_history(third) == 3);
    REQUIRE(core.history_base() == 3);
    REQUIRE(core.retired_through() == third);

    // Faulted: the store cannot write, so the erase of the next three fails.
    const std::uint64_t sixth = ordinal_at(core, 5);
    const std::uint64_t through_before = core.retired_through();
    const std::size_t base_before = core.history_base();
    const std::size_t end_before = core.history_end();
    const std::size_t live_before = core.live().size();
    fault.fail_writes = true;
    const std::size_t retired = core.retire_history(sixth);
    fault.fail_writes = false;
    CHECK(fault.write_faults > 0);                       // the erase reached the fault
    CHECK(retired == 0);
    CHECK(core.retired_through() == through_before);     // not ahead of the window
    CHECK(core.history_base() == base_before);
    CHECK(core.history_end() == end_before);
    CHECK(core.live().size() == live_before);
}

}  // namespace

int main() {
    try {
        failed_codec_retirement_changes_nothing();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "codec retirement failure: %s\n", error.what());
    }
    std::printf("test_core_codec_retire_failure_is_atomic: %d checks, %d failures\n", checks,
                failures);
    return failures == 0 ? 0 : 1;
}
