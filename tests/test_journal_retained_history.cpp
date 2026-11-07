// Oversized extents participate in ordinary retained-window readers,
// retirement, move, and a second spill after the page table/file is released.
#include <pineforge/native_order.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace no = pineforge::native_order;
using Status = pineforge::detail::ScratchStatus;
constexpr std::size_t kResidentEvents = 2048;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

std::vector<std::byte> record(const no::CommandEvent& event) {
    const auto source = no::encode_journal_record(event);
    require(source.size() <= std::numeric_limits<std::size_t>::max(), "record size fits size_t");
    std::vector<std::byte> bytes(static_cast<std::size_t>(source.size()));
    require(source.read(0, {bytes.data(), bytes.size()}) == Status::ok,
            "encode complete retained event oracle");
    return bytes;
}

std::uint64_t ordinal_of(const no::CommandEvent& event) {
    return std::visit([](const auto& value) { return value.ordinal; }, event);
}

struct Window {
    std::size_t base;
    std::vector<std::vector<std::byte>> bytes;
    std::vector<std::uint64_t> ordinals;
};

Window append_window(no::WorkingRequestCore& core, unsigned generation) {
    Window expected{core.history_end(), {}, {}};
    std::vector<std::weak_ptr<const no::RequestDefinition>> owners;
    const auto capture = [&] {
        const auto& event = core.history().back();
        expected.bytes.push_back(record(event));
        expected.ordinals.push_back(ordinal_of(event));
    };
    for (std::size_t i = 0; i < 40; ++i) {
        std::uint64_t incarnation = core.last_incarnation() + 1;
        std::uint64_t ordinal = core.last_ordinal() + 5;
        no::Request request{no::Transact{1.0}, "retained-" + std::to_string(generation),
            std::string(i % 3 == 0 ? 32769 + i * 101 : 31 + i, static_cast<char>('A' + i % 26))};
        request.trigger = no::Limit{40.0 + i};
        const auto accepted = core.submit(request, 9000 + generation, incarnation, ordinal);
        require(accepted.status == no::SubmitStatus::Accepted && accepted.handle,
                "retained request is accepted");
        capture();
        owners.push_back(std::get<no::AcceptedEvent>(core.history().back()).definition);
        ordinal = core.last_ordinal() + 5;
        require(core.cancel(*accepted.handle, ordinal).status == no::CancelStatus::Cancelled,
                "retained request is cancelled");
        capture();
    }
    for (std::size_t i = 0; i < kResidentEvents + 64; ++i) {
        std::uint64_t incarnation = core.last_incarnation() + 1;
        std::uint64_t ordinal = core.last_ordinal() + 5;
        require(core.submit({no::Transact{0.0}, "retained-filler", std::to_string(i)}, 0,
                            incarnation, ordinal).status == no::SubmitStatus::Rejected,
                "one retained filler event");
        capture();
    }
    for (const auto& owner : owners)
        require(owner.expired(), "retained prefix actually spilled before readers start");
    require(expected.bytes.front().size() > 32768, "retained event really spans packing pages");
    return expected;
}

void retained_extents() {
    const no::RunIdentity run{"journal-retained-extents", 1};
    no::WorkingRequestCore core(run);
    const auto first = append_window(core, 1);
    const auto reader = core.history();
    auto oldest = reader.begin();
    auto later = reader.begin() + 72;
    // Copying an event explicitly keeps its value alive across cache
    // eviction/retirement; iterators re-resolve absolute positions on access.
    const no::CommandEvent copied = *oldest;
    for (std::size_t i = 0; i < 80; i += 7)
        require(record(core.history_at(i)) == first.bytes[i], "independent extent reload");
    require(record(*oldest) == first.bytes[0] && record(*later) == first.bytes[72],
            "two readers retain their absolute positions through cache churn");
    require(record(copied) == first.bytes[0], "owning event copy survives cache eviction");

    // Cut at a non-chunk boundary in a spilled extent, using a missing
    // ordinal between two events to exercise upper_bound_position.
    constexpr std::size_t cut = 25;
    const auto old_ordinal = core.last_ordinal();
    const auto old_incarnation = core.last_incarnation();
    require(core.retire_history(first.ordinals[cut - 1] + 1) == cut,
            "partial retirement advances through the requested oversized extent");
    require(core.history_base() == cut && core.history_end() == first.bytes.size()
                && core.retired_through() == first.ordinals[cut - 1]
                && core.last_ordinal() == old_ordinal
                && core.last_incarnation() == old_incarnation,
            "retirement changes only the retained window");
    require(core.event_at({run, first.ordinals[cut - 1]}) == nullptr,
            "retired oversized event no longer resolves by ordinal");
    bool out_of_range = false;
    try { (void)core.history_at(cut - 1); }
    catch (const std::out_of_range&) { out_of_range = true; }
    require(out_of_range, "retired absolute positions are refused");
    require(record(*later) == first.bytes[72], "later reader still resolves a retained extent");
    require(record(copied) == first.bytes[0], "copied event survives retirement");

    no::WorkingRequestCore moved(std::move(core));
    std::size_t position = cut;
    for (const auto& event : moved.history()) {
        require(record(event) == first.bytes[position], "moved journal preserves retained fields");
        ++position;
    }
    require(position == first.bytes.size(), "moved reader sees the complete retained suffix");
    const auto first_end = moved.history_end();
    require(moved.retire_history(moved.last_ordinal()) == first_end - cut
                && moved.history().empty() && moved.history_base() == first_end,
            "full retirement releases the first set of spilled extents");

    // A released page table reuses its page numbers and a fresh data file
    // starts at offset zero, while event positions/ordinals keep ascending.
    const auto second = append_window(moved, 2);
    require(second.base == first_end && moved.history_base() == first_end,
            "second spill keeps absolute event positions");
    position = 0;
    for (const auto& event : moved.history()) {
        require(record(event) == second.bytes[position], "fresh extents cannot hit stale cache keys");
        const auto* found = moved.event_at({run, second.ordinals[position]});
        require(found && record(*found) == second.bytes[position], "fresh ordinal reader");
        ++position;
    }
    require(position == second.bytes.size(), "second reader visits every event");
    const auto end = moved.history_end();
    moved.reset({"journal-retained-extents", 2});
    require(end > first_end && moved.history_base() == 0 && moved.history_end() == 0
                && moved.history().empty(), "reset releases extents and rebinds the run");
}
} // namespace

int main() {
    try {
        retained_extents();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_journal_retained_history: %s\n", error.what());
        return 1;
    }
    return 0;
}
