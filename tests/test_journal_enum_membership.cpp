#include "command_scratch_cache_run_fixture.hpp"
#include "../src/source/admission_journal_codec.hpp"
#include <pineforge/native_order.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
namespace no = pineforge::native_order;
namespace ad = pineforge::admission;
namespace pd = pineforge::detail;
using Status = pd::ScratchStatus;
using Bytes = std::vector<std::byte>;
constexpr std::size_t page_size = command_cache_test::page_bytes;
int failures = 0;

const char* show(Status status) {
    switch (status) {
    case Status::ok: return "ok";
    case Status::end: return "end";
    case Status::cancelled: return "cancelled";
    case Status::not_found: return "not_found";
    case Status::programming_error: return "programming_error";
    case Status::io_error: return "io_error";
    case Status::corrupt: return "corrupt";
    }
    return "unknown";
}
template<class T> auto show(T value) {
    if constexpr (std::is_enum_v<T>) return +static_cast<std::underlying_type_t<T>>(value);
    else return value;
}
template<class A, class B>
bool compare(const std::string& row, A actual, B expected, bool precondition = false) {
    if (actual == expected) return true;
    std::ostringstream message;
    message << row << ": expected=" << show(expected) << " actual=" << show(actual);
    if (precondition) throw std::runtime_error(message.str());
    std::cerr << "FAIL " << message.str() << '\n';
    ++failures;
    return false;
}
template<class F> void row(const char* name, F run) {
    const int before = failures;
    try { run(); }
    catch (const std::exception& e) {
        std::cerr << "NOT COMPARED " << name << ": expected=completed comparison actual=" << e.what() << '\n';
        ++failures;
    }
    if (before == failures) std::cout << "PASS " << name << '\n';
}

struct Store {
    std::array<std::byte, 128 * page_size> bytes{};
    pd::FixedMemoryScratchBacking backing{{bytes.data(), bytes.size()}, page_size};
    pd::CommandScratchStore store{command_cache_test::config(), &backing};
    Store() {
        compare("open store", store.status(), Status::ok, true);
        compare("initial store count", store.record_count(), 0u, true);
        compare("initial transaction active", store.transaction_active(), false, true);
    }
};
template<class Event> Bytes wire(const Event& event) {
    const auto encoded = encode_journal_record(event);
    compare("encoded record has at least one word", encoded.size() >= 8, true, true);
    Bytes bytes(encoded.size());
    compare("read encoded record", encoded.read(0, {bytes.data(), bytes.size()}), Status::ok, true);
    return bytes;
}
// These pairs differ in the low byte of the enum word. Inspect all eight
// bytes and the suffix too: an empty, stale or otherwise damaged record is
// not evidence that an undeclared member was refused.
Bytes changed_word(const Bytes& good, Bytes bad, std::uint64_t from, std::uint64_t to) {
    compare("paired record size", bad.size(), good.size(), true);
    const auto at = std::mismatch(good.begin(), good.end(), bad.begin()).first - good.begin();
    compare("changed word fits", good.size() - at >= 8, true, true);
    for (unsigned i = 0; i < 8; ++i) {
        compare("original word byte", std::to_integer<unsigned>(good[at+i]),
                (from >> (8*i)) & 255, true);
        compare("changed word byte", std::to_integer<unsigned>(bad[at+i]),
                (to >> (8*i)) & 255, true);
    }
    compare("bytes following changed word", std::equal(good.begin()+at+8, good.end(),
                                                        bad.begin()+at+8), true, true);
    return bad;
}
pd::ScratchSpanSource source(const Bytes& bytes) { return pd::ScratchSpanSource({bytes.data(), bytes.size()}); }

no::CommandEvent cause(no::RunIdentity run, std::uint64_t ordinal, no::GroupEffect effect) {
    no::RequestDefinition def;
    def.handle = {run, 99};
    def.request.group = no::Member{7, 1, effect};
    no::ExecutionAppliedEvent event;
    event.ordinal = ordinal;
    event.definition = std::make_shared<const no::RequestDefinition>(def);
    event.terminal = false;
    event.filled_working = 0.0;
    return event;
}
void native() {
    Store memory;
    no::WorkingRequestCore core{no::RunIdentity{no::SessionKey("run-a"), 1}};
    no::bind_journal_codec(core, memory.store);
    std::uint64_t inc = 1, next = 1;
    no::Request sibling{no::Flatten{}, "s", ""};
    sibling.group = no::Member{7, 2, no::GroupEffect::Cancel};
    const auto submitted = core.submit(sibling, 0, inc, next);
    compare("sibling submit", submitted.status, no::SubmitStatus::Accepted, true);
    compare("sibling handle present", submitted.handle.has_value(), true, true);
    const auto handle = *submitted.handle;
    const auto live_flatten = [&] {
        const auto* live = core.find_live(handle);
        return live && std::holds_alternative<no::RemainingFlattenAll>(live->remaining);
    };
    compare("live flatten sibling", live_flatten(), true, true);
    std::array<std::byte, page_size> page{};
    const pd::ScratchBuffer buffer{page.data(), page.size()};
    for (auto effect : {no::GroupEffect::Cancel, no::GroupEffect::Reduce}) {
        const auto id = next++;
        const auto bytes = wire(cause(core.identity(), id, effect));
        compare("declared effect append", no::append_journal_record(core, source(bytes)), Status::ok, true);
        const auto* found = core.event_at({core.identity(), id});
        const auto* event = found ? std::get_if<no::ExecutionAppliedEvent>(found) : nullptr;
        compare("applied control has definition", event && event->definition, true, true);
        const auto* member = std::get_if<no::Member>(&event->definition->request.group);
        compare("applied control has member", member != nullptr, true, true);
        compare("control source handle", event->definition->handle == no::RequestHandle{core.identity(), 99}, true, true);
        compare("control group", member->group, 7u, true);
        compare("control cohort", member->cohort, 1, true);
        compare("control effect", member->effect, effect, true);
        compare("control terminal", event->terminal, false, true);
        compare("control filled", event->filled_working, 0.0, true);
        const auto result = core.apply_group_effect({core.identity(), id}, handle, next);
        const auto* unchanged = std::get_if<no::NoChange>(&result);
        compare("declared effect gives NoChange", unchanged != nullptr, true, true);
        compare("declared effect reason", unchanged->reason, no::NoChangeReason::NoTransition, true);
        compare("declared effect preserves sibling", live_flatten(), true, true);
    }
    const auto end = core.history_end();
    compare("accepted sibling and both causes", end, 3u, true);
    const auto good = wire(cause(core.identity(), next, no::GroupEffect::Cancel));
    const auto bad = changed_word(good, wire(cause(core.identity(), next, static_cast<no::GroupEffect>(2))), 0, 2);
    compare("native GroupEffect=2 append", no::append_journal_record(core, source(bad)), Status::corrupt);
    no::CommandEvent output = no::NativeRiskEvent{777};
    compare("native GroupEffect=2 decode", no::decode_journal_record(source(bad), buffer, output), Status::corrupt);
    const auto* sentinel = std::get_if<no::NativeRiskEvent>(&output);
    compare("native decode output ordinal", sentinel ? sentinel->ordinal : 0, 777u);
    compare("native retained records", memory.store.record_count(), end);
    compare("native history end", core.history_end(), end);
    compare("native last ordinal", core.last_ordinal(), next - 1);
    compare("native refused ordinal absent", core.event_at({core.identity(), next}) == nullptr, true);
    auto following = next + 1;
    const auto applied = core.apply_group_effect({core.identity(), next}, handle, following);
    const auto* error = std::get_if<no::PreparationError>(&applied);
    compare("absent cause gives InvalidCause", error && error->code == no::CoreFailure::InvalidCause, true);
    compare("native live sibling", live_flatten(), true);
    compare("native group receipts", core.group_effect_receipt_count(), 0u);
    compare("native empty input", no::append_journal_record(core, source({})), Status::corrupt);
    compare("native retry declared effect at refused ordinal", no::append_journal_record(core, source(good)), Status::ok);
}

void native_surface_and_refusal() {
    Store memory;
    no::WorkingRequestCore core{no::RunIdentity{"run-a", 1}};
    no::bind_journal_codec(core, memory.store);
    no::Request request{no::Flatten{}, "s", ""};
    request.group = no::Member{7, 1, static_cast<no::GroupEffect>(2)};
    std::uint64_t inc = 1, next = 1;
    const auto result = core.submit(request, 0, inc, next);
    compare("attempt refused", result.status, no::SubmitStatus::Rejected, true);
    compare("refusal reason", result.reason == no::RequestRejectReason::InvalidGroup, true, true);
    const auto refused = core.history_at(0);
    const auto* rejection = std::get_if<no::RejectedEvent>(&refused);
    compare("stored refusal is RejectedEvent", rejection != nullptr, true, true);
    std::array<std::byte, page_size> page{};
    const pd::ScratchBuffer buffer{page.data(), page.size()};
    no::CommandEvent out;
    auto valid = *rejection;
    valid.ordinal = next;
    valid.request.group = no::Member{7, 1, no::GroupEffect::Cancel};
    const auto good = wire(no::CommandEvent{valid});
    compare("valid rejection decode", no::decode_journal_record(source(good), buffer, out), Status::ok, true);
    compare("declared rejection matches its input", wire(out) == good, true, true);
    auto invalid = valid;
    invalid.surface = static_cast<no::CommandSurface>(255);
    const auto bad = changed_word(good, wire(no::CommandEvent{invalid}), 0, 255);
    compare("native CommandSurface=255 append", no::append_journal_record(core, source(bad)), Status::corrupt);
    compare("native CommandSurface=255 decode", no::decode_journal_record(source(bad), buffer, out), Status::corrupt);
    valid.ordinal = core.last_ordinal() + 1; // A fresh ordinal even on the unfixed base.
    invalid = valid;
    invalid.request = request;
    const auto attempted = changed_word(wire(no::CommandEvent{valid}), wire(no::CommandEvent{invalid}), 0, 2);
    compare("external refusal import with bad attempted effect", no::append_journal_record(core, source(attempted)),
            Status::corrupt);
    compare("external refusal with bad attempted effect", no::decode_journal_record(source(attempted), buffer, out),
            Status::corrupt);
    // Evict the core's four cached events. Its own refusal is evidence of
    // the actual bad request, never an executable definition or raw import.
    next = core.last_ordinal() + 1;
    for (int i = 0; i < 4; ++i) {
        const auto fill_cache = core.submit(no::Request{no::Flatten{}, "other", ""}, 0, inc, next);
        compare("cache eviction submit", fill_cache.status, no::SubmitStatus::Accepted, true);
    }
    const auto* stored = std::get_if<no::RejectedEvent>(&core.history_at(0));
    compare("reloaded refusal is RejectedEvent", stored != nullptr, true, true);
    const auto* member = std::get_if<no::Member>(&stored->request.group);
    compare("reloaded attempted group present", member != nullptr, true, true);
    compare("reloaded refusal reason", stored->reason, no::RequestRejectReason::InvalidGroup);
    compare("reloaded attempted effect", static_cast<unsigned>(member->effect), 2u);
}

void admission(const char* field, const ad::Event& good_event, Bytes malformed, std::uint64_t from,
               std::uint64_t to, ad::Journal& journal, Store& memory) {
    const std::string label(field);
    const auto good = wire(good_event);
    const auto bad = changed_word(good, std::move(malformed), from, to);
    std::array<std::byte, page_size> page{};
    const pd::ScratchBuffer buffer{page.data(), page.size()};
    const auto seq = ad::sequence(good_event);
    // The fixtures contain only ordinary numbers or the default absent NaN.
    // For them the canonical fold is exactly FNV-1a of the wire after the
    // row-count word (the count is storage framing, not a reflected field).
    constexpr std::uint64_t seed = 12345;
    auto expected_digest = seed;
    for (auto i = good.begin() + 8; i != good.end(); ++i)
        expected_digest = (expected_digest ^ std::to_integer<unsigned>(*i)) * 1099511628211ULL;
    auto digest = seed;
    compare("declared standalone fold", ad::fold_journal_record(source(good), buffer, digest), Status::ok, true);
    compare("declared fold digest", digest, expected_digest, true);
    ad::JournalRecordSummary control;
    digest = seed;
    compare("declared window fold", ad::fold_validated_journal_record(source(good), buffer, control, digest),
            Status::ok, true);
    compare("declared fold sequence", control.sequence, seq, true);
    compare("declared window digest", digest, expected_digest, true);
    ad::Event output;
    compare("declared admission decode", ad::decode_journal_record(source(good), buffer, output), Status::ok, true);
    compare("decoded declared record matches input bytes", wire(output) == good, true, true);
    compare("allocated sequence", seq, 1u, true);
    compare("outstanding allocation", journal.has_unfinished_events(), true, true);

    const auto appended = ad::append_journal_record(journal, source(bad));
    compare(label + " append", appended, Status::corrupt);
    compare(label + " decode", ad::decode_journal_record(source(bad), buffer, output), Status::corrupt);
    compare(label + " decode preserves output", wire(output) == good, true);
    for (const auto frontier : {std::uint64_t{0}, seq}) {
        ad::JournalRecordSummary summary{777, -17, true};
        auto digest = seed;
        compare(label + " validated fold", ad::fold_validated_journal_record(source(bad), buffer, summary,
                                                                             digest, frontier), Status::corrupt);
        compare(label + " fold sequence", summary.sequence, 777u);
        compare(label + " fold rejection bar", summary.rejected_bar, -17);
        compare(label + " fold rejection flag", summary.rejected, true);
        compare(label + " fold digest", digest, seed);
    }
    digest = seed;
    compare(label + " standalone fold", ad::fold_journal_record(source(bad), buffer, digest), Status::corrupt);
    compare(label + " standalone digest", digest, seed);
    compare(label + " event count", journal.events().size(), 0u);
    compare(label + " stored count", memory.store.record_count(), 0u);
    compare(label + " sequence still outstanding", journal.has_unfinished_events(), true);
    compare(label + " empty input", ad::append_journal_record(journal, source({})), Status::corrupt);
    compare(label + " retry needs an outstanding sequence", journal.has_unfinished_events(), true, true);
    compare(label + " retry declared value", ad::append_journal_record(journal, source(good)), Status::ok);
    const auto& events = journal.events();
    compare("one event after retry", events.size(), 1u, true);
    compare(label + " stored declared event", wire(events.front()) == good, true);
    compare(label + " sequence consumed", journal.has_unfinished_events(), false);
}
void admission_command(bool outcome) {
    Store memory;
    ad::Journal journal;
    ad::bind_journal_codec(journal, memory.store);
    auto observation = std::make_shared<ad::CommandObservation>();
    observation->command = journal.next_sequence();
    observation->kind = ad::CommandKind::Raw;
    observation->id = "order-944";
    const ad::CommandEvent good{observation, ad::Outcome::Admitted};
    auto bad = good;
    if (outcome) {
        bad.outcome = static_cast<ad::Outcome>(99);
        admission("admission Outcome=99", good, wire(ad::Event{bad}), 0, 99, journal, memory);
    } else {
        auto changed = std::make_shared<ad::CommandObservation>(*observation);
        changed->kind = static_cast<ad::CommandKind>(-1);
        bad.observation = changed;
        admission("admission CommandKind=-1", good, wire(ad::Event{bad}), 1, UINT64_MAX, journal, memory);
    }
}
void admission_review(bool checkpoint) {
    Store memory;
    ad::Journal journal;
    ad::bind_journal_codec(journal, memory.store);
    ad::ReviewEvent good;
    good.receipt = {journal.next_sequence(), ad::Checkpoint::TerminalGross, 7, 0};
    good.resolutions.push_back({71, ad::ResolutionKind::PairedTransaction, 72});
    auto bad = good;
    if (checkpoint) bad.receipt.checkpoint = static_cast<ad::Checkpoint>(99);
    else bad.resolutions.front().kind = static_cast<ad::ResolutionKind>(99);
    admission(checkpoint ? "admission Checkpoint=99" : "admission ResolutionKind=99",
              good, wire(ad::Event{bad}), 2, 99, journal, memory);
}
void admission_cursor(const char* field) {
    Store memory;
    ad::Journal journal;
    ad::bind_journal_codec(journal, memory.store);
    auto observation = std::make_shared<ad::CommandObservation>();
    observation->command = journal.next_sequence();
    const ad::Event good = ad::CommandEvent{observation, ad::Outcome::Admitted};
    // OrderBirth exposes no invalid cursor constructor. Address its named
    // field on the wire; changed_word checks that only None (0) became 99.
    auto bad = wire(good);
    const std::string path = std::string("event.observation.birth.") + field;
    const auto* chars = reinterpret_cast<const char*>(bad.data());
    const std::string_view view(chars, bad.size());
    const auto at = view.find(path);
    compare("cursor path found", at != view.npos, true, true);
    compare("cursor path unique", view.find(path, at + 1) == view.npos, true, true);
    const auto offset = at + path.size() + 8; // skip the field's type tag
    compare("cursor word fits", offset + 8 <= bad.size(), true, true);
    bad[offset] = std::byte{99};
    admission(field, good, std::move(bad), 0, 99, journal, memory);
}
} // namespace

int main() {
    row("native group effect", native);
    row("native surface and rejected request", native_surface_and_refusal);
    row("admission outcome", [] { admission_command(true); });
    row("admission command kind", [] { admission_command(false); });
    row("admission checkpoint", [] { admission_review(true); });
    row("admission resolution", [] { admission_review(false); });
    row("admission cursor domain", [] { admission_cursor("cursor_domain"); });
    row("admission cursor position", [] { admission_cursor("cursor_position"); });
    if (failures) return 1;
    std::cout << "PASS #944 journal enum validation\n";
    return 0;
}
