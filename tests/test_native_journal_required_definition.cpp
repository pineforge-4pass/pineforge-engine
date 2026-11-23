#include "command_scratch_cache_run_fixture.hpp"
#include <pineforge/native_order.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <variant>

namespace {
namespace no = pineforge::native_order;
namespace pd = pineforge::detail;
using Status = pd::ScratchStatus;

enum class Check { setup, result };
struct Mismatch {};

template<class T, class U>
void expect(Check kind, const char* what, T actual, U expected) {
    if (actual == expected) return;
    std::cerr << (kind == Check::setup ? "NOT COMPARED" : "FAIL") << ' ' << what
              << ": expected=" << static_cast<std::uint64_t>(expected)
              << " actual=" << static_cast<std::uint64_t>(actual) << '\n';
    throw Mismatch{};
}

no::RequestHandle submit(no::WorkingRequestCore& core, const no::Request& request,
                         std::uint64_t& incarnation, std::uint64_t& ordinal) {
    const auto result = core.submit(request, 0, incarnation, ordinal);
    expect(Check::setup, request.label.c_str(), result.status, no::SubmitStatus::Accepted);
    expect(Check::setup, "accepted handle present", result.handle.has_value(), true);
    return *result.handle;
}

void check() {
    constexpr auto page_size = command_cache_test::page_bytes;
    std::array<std::byte, 256 * page_size> storage{};
    pd::FixedMemoryScratchBacking backing({storage.data(), storage.size()}, page_size);
    pd::CommandScratchStore store(command_cache_test::config(), &backing);
    expect(Check::setup, "scratch status", store.status(), Status::ok);
    const no::RunIdentity run{no::SessionKey("run-a"), 1};
    no::WorkingRequestCore core{run};
    no::bind_journal_codec(core, store);

    std::uint64_t incarnation = 1, ordinal = 1;
    const auto parent = submit(core, {no::Transact{1}, "parent", ""}, incarnation, ordinal);
    no::Request request{no::Reduce{no::OwnerOpenedUnits{}}, "child", ""};
    request.owner = no::WaitForApplied{parent};
    const auto child = submit(core, request, incarnation, ordinal);
    const auto* row = core.find_live(child);
    expect(Check::setup, "child live", row != nullptr, true);
    const auto* wait = std::get_if<no::Wait>(&row->authority);
    expect(Check::setup, "child has Wait authority", wait != nullptr, true);
    expect(Check::setup, "child waits for submitted parent", wait->parent == parent, true);
    expect(Check::setup, "history before append", core.history_end(), 2u);
    expect(Check::setup, "stored records before append", store.record_count(), 2u);
    expect(Check::setup, "last ordinal before append", core.last_ordinal(), 2u);
    expect(Check::setup, "new event ordinal", ordinal, 3u);

    no::ExecutionAppliedEvent applied;
    applied.ordinal = ordinal;
    const no::CommandEvent event = applied;
    const auto source = no::encode_journal_record(event);
    // The complete issue input: 32 LE64 words and three one-byte flags
    // (definition, terminal, terminal reason). A truncated record is a different refusal.
    expect(Check::setup, "applied record bytes", source.size(), 32u * 8 + 3);
    // Two LE64 words (alternative, ordinal), then the definition presence byte.
    std::array<std::byte, 17> prefix{};
    expect(Check::setup, "read applied prefix", source.read(0, {prefix.data(), prefix.size()}), Status::ok);
    std::array<std::byte, 17> expected{};
    expected[0] = static_cast<std::byte>(event.index());
    expected[8] = static_cast<std::byte>(ordinal);
    for (std::size_t i = 0; i < prefix.size(); ++i)
        expect(Check::setup, "applied tag / ordinal / absent definition byte",
               std::to_integer<unsigned>(prefix[i]), std::to_integer<unsigned>(expected[i]));

    const auto status = no::append_journal_record(core, source);
    expect(Check::result, "append without definition", status, Status::corrupt);
    expect(Check::result, "history after refusal", core.history_end(), 2u);
    expect(Check::result, "stored records after refusal", store.record_count(), 2u);
    expect(Check::result, "last ordinal after refusal", core.last_ordinal(), 2u);
    expect(Check::result, "refused event visible", core.event_at({run, ordinal}) != nullptr, false);

    // Use a real accepted event as the output witness, so a default result cannot pass.
    no::CommandEvent output = core.history_at(0);
    const auto* accepted = std::get_if<no::AcceptedEvent>(&output);
    expect(Check::setup, "parent acceptance has definition", accepted && accepted->definition, true);
    expect(Check::setup, "parent acceptance handle", accepted->handle() == parent, true);
    const auto parent_definition = accepted->definition;
    std::array<std::byte, page_size> page{};
    const auto decoded = no::decode_journal_record(source, {page.data(), page.size()}, output);
    expect(Check::result, "decode without definition", decoded, Status::corrupt);
    accepted = std::get_if<no::AcceptedEvent>(&output);
    expect(Check::result, "decode retained parent acceptance",
           accepted && accepted->ordinal == 1 && accepted->definition == parent_definition, true);

    // Same applied event and ordinal, with only the missing field supplied.
    applied.definition = parent_definition;
    const no::CommandEvent repaired = applied;
    const auto valid = no::encode_journal_record(repaired);
    const auto appended = no::append_journal_record(core, valid);
    expect(Check::result, "append with definition", appended, Status::ok);
    expect(Check::result, "history after valid append", core.history_end(), 3u);
    expect(Check::result, "stored records after valid append", store.record_count(), 3u);
    const auto* stored = core.event_at({run, ordinal});
    const auto* fill = stored ? std::get_if<no::ExecutionAppliedEvent>(stored) : nullptr;
    expect(Check::result, "applied record with definition present", fill && fill->definition, true);
    expect(Check::result, "stored applied ordinal", fill->ordinal, ordinal);
    expect(Check::result, "stored applied handle", fill->handle() == parent, true);

    // The other shared pointer is optional: this attempted order lists its opening inline.
    request.owner = no::BindOpenings{{parent}, 1, {}};
    const no::CommandEvent rejected = no::RejectedEvent{4, request, no::RequestRejectReason::InvalidOwner};
    const auto inline_openings = no::encode_journal_record(rejected);
    expect(Check::result, "append inline openings", no::append_journal_record(core, inline_openings), Status::ok);
    const auto* retained = core.event_at({run, 4});
    const auto* refusal = retained ? std::get_if<no::RejectedEvent>(retained) : nullptr;
    const auto* owner = refusal ? std::get_if<no::BindOpenings>(&refusal->request.owner) : nullptr;
    expect(Check::result, "inline openings without external readback", owner && !owner->external_openings, true);
    expect(Check::result, "inline opening count", owner->openings.size(), 1u);
    expect(Check::result, "inline opening handle", owner->openings.front() == parent, true);
}
} // namespace

int main() {
    try {
        check();
    } catch (const Mismatch&) {
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "NOT COMPARED journal fixture: expected=normal return actual=" << e.what() << '\n';
        return 1;
    }
    std::puts("PASS missing definition refused; ordinal 3 retained only with the parent definition");
    return 0;
}
