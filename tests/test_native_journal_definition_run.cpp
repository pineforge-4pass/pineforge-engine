// #967: a raw journal import must validate the run of each definition's own handle.
#include "command_scratch_arena_fixture.hpp"
#include <pineforge/native_order.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace {
namespace no = pineforge::native_order;
namespace pd = pineforge::detail;
using Status = pd::ScratchStatus;
constexpr auto page_size = command_arena_test::kPageBytes;

struct Mismatch { int exit_code; };

std::ostream& operator<<(std::ostream& out, Status status) {
    if (status == Status::ok) return out << "ok";
    if (status == Status::corrupt) return out << "corrupt";
    return out << static_cast<unsigned>(status);
}

template<class A, class B>
void equal(const char* kind, const char* row, const A& actual, const B& expected) {
    if (actual == expected) return;
    std::cerr << kind << ": " << row << ": expected=" << expected << " actual=" << actual << '\n';
    throw Mismatch{std::string_view(kind) == "FAIL" ? 1 : 2};
}

// An actual submission supplies the seed and the foreign AcceptedEvent.
no::DefinitionRef submit(no::WorkingRequestCore& core, const no::Request& request,
                         std::uint64_t id, std::uint64_t ordinal) {
    std::uint64_t incarnation = id, next = ordinal;
    const auto result = core.submit(request, 0, incarnation, next);
    equal("NOT COMPARED", "submit status", int(result.status), int(no::SubmitStatus::Accepted));
    equal("NOT COMPARED", "submitted rows", core.history_end(), 1u);
    const auto* accepted = std::get_if<no::AcceptedEvent>(&core.history_at(0));
    equal("NOT COMPARED", "submitted AcceptedEvent with definition",
          accepted && accepted->definition, true);
    equal("NOT COMPARED", "submitted ordinal", accepted->ordinal, ordinal);
    equal("NOT COMPARED", "submitted session", accepted->handle().run.session_key,
          core.identity().session_key);
    equal("NOT COMPARED", "submitted run", accepted->handle().run.run_number,
          core.identity().run_number);
    equal("NOT COMPARED", "submitted incarnation", accepted->handle().incarnation, id);
    return accepted->definition;
}

// Supply a local definition to the input scan, then a foreign one to the store.
class SwitchingRecord final : public pd::ScratchByteSource {
public:
    SwitchingRecord(const pd::ScratchByteSource& local, const pd::ScratchByteSource& foreign)
        : local_(local), foreign_(foreign) {}
    std::uint64_t size() const noexcept override { return local_.size(); }
    Status read(std::uint64_t offset, pd::ScratchBuffer out) const noexcept override {
        if (!offset && out.size) ++starts_;
        return (switched() ? foreign_ : local_).read(offset, out);
    }
    bool switched() const noexcept { return starts_ >= 2; }
private:
    const pd::ScratchByteSource& local_;
    const pd::ScratchByteSource& foreign_;
    mutable unsigned starts_ = 0;
};

void test() {
    // The differing session byte is beyond both an embedded NUL and the first page.
    std::string key(page_size + 17, 'a');
    key[3] = '\0';
    const no::RunIdentity run{key, 7};
    key.back() = 'b';
    const no::RunIdentity other_session{key, 7};
    no::WorkingRequestCore other({run.session_key, 8});
    const no::Request request{no::Transact{1.0}, "issue-967", ""};
    const auto foreign = submit(other, request, 2, 11);

    std::array<std::byte, 256 * page_size> storage{};
    pd::FixedMemoryScratchBacking backing({storage.data(), storage.size()}, page_size);
    pd::CommandScratchStore store(command_arena_test::config(), &backing);
    equal("NOT COMPARED", "scratch store status", store.status(), Status::ok);
    no::WorkingRequestCore destination(run);
    no::bind_journal_codec(destination, store);
    const auto local = submit(destination, request, 1, 10);
    equal("NOT COMPARED", "seed store count", store.record_count(), 1u);
    equal("NOT COMPARED", "seed last ordinal", destination.last_ordinal(), 10u);

    const auto digest = [&] {
        std::array<std::byte, page_size> page{};
        std::uint64_t value = 0;
        equal("NOT COMPARED", "read committed journal digest",
              store.state_digest({page.data(), page.size()}, value), Status::ok);
        return value;
    };
    const auto before = digest();
    const auto refused_append = [&](const char* row, Status status) {
        equal("FAIL", row, status, Status::corrupt);
        equal("FAIL", "transaction active after refusal", store.transaction_active(), false);
        equal("FAIL", "journal end after refusal", destination.history_end(), 1u);
        equal("FAIL", "last ordinal after refusal", destination.last_ordinal(), 10u);
        equal("FAIL", "stored records after refusal", store.record_count(), 1u);
        equal("FAIL", "committed journal after refusal", digest(), before);
    };
    const auto reject = [&](const char* row, const pd::ScratchByteSource& source) {
        refused_append(row, no::append_journal_record(destination, source));
    };

    const auto source = no::encode_journal_record(other.history_at(0));
    equal("NOT COMPARED", "foreign encoded record exceeds a page", source.size() > page_size, true);
    // Decode the exact input independently of append: malformed or stale input is no witness.
    std::array<std::byte, page_size> page{};
    no::CommandEvent decoded;
    equal("NOT COMPARED", "decode foreign record",
          no::decode_journal_record(source, {page.data(), page.size()}, decoded), Status::ok);
    const auto* candidate = std::get_if<no::AcceptedEvent>(&decoded);
    equal("NOT COMPARED", "encoded AcceptedEvent with definition",
          candidate && candidate->definition, true);
    equal("NOT COMPARED", "encoded ordinal", candidate->ordinal, 11u);
    equal("NOT COMPARED", "encoded session", candidate->handle().run.session_key, run.session_key);
    equal("NOT COMPARED", "encoded run", candidate->handle().run.run_number, 8u);
    reject("foreign AcceptedEvent (run number)", source);

    const pd::ScratchSpanSource empty({nullptr, 0});
    reject("empty record", empty);
    // The local and foreign definitions differ in session only; put the latter in each slot.
    auto different = std::make_shared<no::RequestDefinition>(*local);
    different->handle.run = other_session;
    const no::CommandEvent bad_first = no::ReplacedEvent{11, different, local};
    const no::CommandEvent bad_second = no::ReplacedEvent{11, local, different};
    reject("foreign replacement predecessor definition", no::encode_journal_record(bad_first));
    reject("foreign replacement successor definition", no::encode_journal_record(bad_second));

    const auto append = [&](const char* row, const no::CommandEvent& event) -> const no::CommandEvent& {
        const auto position = destination.history_end();
        const auto ordinal = std::visit([](const auto& e) { return e.ordinal; }, event);
        equal("FAIL", row, no::append_journal_record(destination, no::encode_journal_record(event)),
              Status::ok);
        equal("FAIL", "journal end after import", destination.history_end(), position + 1);
        equal("FAIL", "stored records after import", store.record_count(), position + 1);
        equal("FAIL", "last ordinal after import", destination.last_ordinal(), ordinal);
        const auto& stored = destination.history_at(position);
        equal("FAIL", "stored event kind", stored.index(), event.index());
        equal("FAIL", "stored ordinal", std::visit([](const auto& e) { return e.ordinal; }, stored), ordinal);
        return stored;
    };
    // Repeat the issue input at the refused ordinal, changing only its definition's run.
    auto same_run = std::make_shared<no::RequestDefinition>(*foreign);
    same_run->handle.run = run;
    const no::CommandEvent valid = no::AcceptedEvent{11, same_run, candidate->surface};
    const auto local_source = no::encode_journal_record(valid);
    equal("NOT COMPARED", "local and foreign record sizes", local_source.size(), source.size());
    SwitchingRecord switching(local_source, source);
    const auto staged_status = no::append_journal_record(destination, switching);
    equal("NOT COMPARED", "store was supplied the foreign record", switching.switched(), true);
    refused_append("foreign AcceptedEvent staged after local input", staged_status);
    const auto& accepted = std::get<no::AcceptedEvent>(
        append("same-run AcceptedEvent", valid));
    equal("FAIL", "stored same-run handle", accepted.definition && accepted.handle() == same_run->handle, true);
    const no::RequestHandle target{other_session, 3};
    auto linked = std::make_shared<no::RequestDefinition>(*same_run);
    linked->handle.incarnation = 3;
    linked->birth.acceptance_ordinal = 12;
    linked->predecessor = foreign->handle;
    linked->root = target;
    const auto& replacement = std::get<no::ReplacedEvent>(
        append("local definitions with foreign links", no::ReplacedEvent{12, same_run, linked}));
    equal("FAIL", "stored both local definitions",
          replacement.predecessor_definition && replacement.successor_definition, true);
    equal("FAIL", "stored predecessor handle", replacement.predecessor() == same_run->handle, true);
    equal("FAIL", "stored successor handle", replacement.successor() == linked->handle, true);
    equal("FAIL", "stored predecessor link", replacement.successor_definition->predecessor == linked->predecessor, true);
    equal("FAIL", "stored root link", replacement.successor_definition->root == linked->root, true);

    // Import a real refused replace whose target is foreign to both cores.
    std::uint64_t incarnation = 3, ordinal = 13;
    const auto refused = other.replace(target, request, 0, incarnation, ordinal);
    equal("NOT COMPARED", "foreign replace status", int(refused.status), int(no::ReplaceStatus::InvalidHandle));
    equal("NOT COMPARED", "foreign replace journal end", other.history_end(), 2u);
    const auto& event = other.history_at(1);
    const auto* invalid = std::get_if<no::InvalidHandleEvent>(&event);
    equal("NOT COMPARED", "kernel recorded the foreign target", invalid && invalid->target == target, true);
    equal("NOT COMPARED", "foreign replace ordinal", invalid->ordinal, 13u);
    const auto& stored = std::get<no::InvalidHandleEvent>(append("foreign event target", event));
    equal("FAIL", "stored foreign target", stored.target == target, true);
}
} // namespace

int main() {
    try {
        test();
    } catch (const Mismatch& mismatch) {
        return mismatch.exit_code;
    } catch (const std::exception& error) {
        std::cerr << "NOT COMPARED: journal exercise: expected=completed actual=" << error.what() << '\n';
        return 2;
    }
    std::cout << "PASS: native journal checks each definition's own run\n";
    return 0;
}
