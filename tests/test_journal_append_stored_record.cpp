// #972: switch equal-sized records at the second read of offset zero.
#include "command_scratch_cache_run_fixture.hpp"
#include <pineforge/detail/journal_codec.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/source/market_admission.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <variant>
#include <vector>

namespace {
namespace no = pineforge::native_order;
namespace ad = pineforge::admission;
namespace pd = pineforge::detail;
using Status = pd::ScratchStatus;
using Bytes = std::vector<std::byte>;
using command_cache_test::bytes;

struct Stop {};
template<class A, class B>
void equal(const char* kind, const char* what, A actual, B expected) {
    const auto a = static_cast<long long>(actual), b = static_cast<long long>(expected);
    if (a != b) {
        std::fprintf(stderr, "%s: %s: actual=%lld expected=%lld\n", kind, what, a, b);
        throw Stop{};
    }
}
template<class A, class B>
void need(const char* what, A actual, B expected) { equal("NOT COMPARED", what, actual, expected); }
template<class A, class B>
void check(const char* what, A actual, B expected) { equal("FAIL", what, actual, expected); }

struct Rig {
    static constexpr auto page_size = command_cache_test::page_bytes;
    Bytes space = Bytes(256 * page_size);
    pd::FixedMemoryScratchBacking backing{{space.data(), space.size()}, page_size};
    pd::CommandScratchStore store{command_cache_test::config(), &backing};

    Rig() { need("scratch store opened", store.status(), Status::ok); }
    std::uint64_t digest() const {
        std::array<std::byte, page_size> page{};
        std::uint64_t value = 0;
        need("store state digest", store.state_digest({page.data(), page.size()}, value), Status::ok);
        return value;
    }
    void stored(std::uint64_t key, const Bytes& expected) {
        pd::ScratchRecordInfo info;
        check("exact storage key", pd::journal_codec::find(store, key, info), Status::ok);
        check("stored byte count", info.value_bytes, expected.size());
        Bytes actual(expected.size());
        check("read stored bytes", store.read(info.handle, pd::ScratchField::value, 0,
              {actual.data(), actual.size()}), Status::ok);
        check("stored bytes equal expected record", actual == expected, true);
    }
};

Bytes wire(const pd::ScratchByteSource& source) {
    if (source.size() <= 8) need("minimum bytes for tag and payload", source.size(), 9);
    Bytes result(static_cast<std::size_t>(source.size()));
    need("copy encoded fixture", source.read(0, {result.data(), result.size()}), Status::ok);
    return result;
}
template<class Event>
Event unpack(const Bytes& image) {
    Event event;
    std::array<std::byte, Rig::page_size> page{};
    need("fixture decodes", decode_journal_record(pd::ScratchSpanSource(bytes(image)),
         {page.data(), page.size()}, event), Status::ok);
    return event;
}
Bytes native(std::uint64_t ordinal) {
    const no::CommandEvent event = no::RejectedEvent{ordinal, no::Request{no::Transact{0}, "raw", ""}};
    auto image = wire(no::encode_journal_record(event));
    const auto decoded = unpack<no::CommandEvent>(image);
    need("native fixture variant", decoded.index(), event.index());
    need("native fixture ordinal", std::get<no::RejectedEvent>(decoded).ordinal, ordinal);
    return image;
}
Bytes admission(std::uint64_t sequence, ad::Outcome outcome) {
    auto observation = std::make_shared<ad::CommandObservation>();
    observation->command = sequence;
    observation->bar = 5;
    const ad::Event event = ad::CommandEvent{observation, outcome, 0, {}, {}};
    auto image = wire(ad::encode_journal_record(event));
    const auto decoded = unpack<ad::Event>(image);
    need("admission fixture variant", decoded.index(), event.index());
    const auto& command = std::get<ad::CommandEvent>(decoded);
    need("admission fixture observation", bool(command.observation), true);
    need("admission fixture sequence", command.observation->command, sequence);
    need("admission fixture bar", command.observation->bar, 5);
    need("admission fixture outcome", command.outcome, outcome);
    return image;
}

class SwitchSource final : public pd::ScratchByteSource {
    pd::ScratchSpanSource a_, b_;
    mutable unsigned starts_ = 0;
public:
    SwitchSource(const Bytes& a, const Bytes& b) : a_(bytes(a)), b_(bytes(b)) {
        need("A and B byte counts", a.size(), b.size());
        need("A and B differ", a != b, true);
    }
    std::uint64_t size() const noexcept override { return a_.size(); }
    Status read(std::uint64_t offset, pd::ScratchBuffer out) const noexcept override {
        if (!offset && out.size) ++starts_;
        return (starts_ < 2 ? a_ : b_).read(offset, out);
    }
    void compared() const { if (starts_ < 2) need("offset-zero reads needed to serve B", starts_, 2); }
};

void invalid_native() {
    Rig rig;
    no::WorkingRequestCore core(no::RunIdentity{"issue-972", 1});
    no::bind_journal_codec(core, rig.store);
    const auto a = native(1);
    auto b = a;
    std::fill_n(b.begin(), 8, std::byte{0xff}); // Out-of-range CommandEvent alternative.
    const pd::ScratchSpanSource good(bytes(a)), bad(bytes(b));
    std::array<std::byte, Rig::page_size> page{};
    no::CommandEvent decoded;
    need("B has invalid tag", no::decode_journal_record(bad, {page.data(), page.size()}, decoded),
         Status::corrupt);
    const auto before = rig.digest();
    SwitchSource input(a, b);
    const auto status = no::append_journal_record(core, input);
    input.compared();
    check("native invalid staged variant", status, Status::corrupt);
    check("invalid variant closed transaction", rig.store.transaction_active(), false);
    check("invalid variant preserved store", rig.digest() == before, true);
    check("no invalid history row", core.history_end(), 0);
    check("no ordinal consumed", core.last_ordinal(), 0);
    check("retry native ordinal 1", no::append_journal_record(core, good), Status::ok);
    check("one native row on retry", core.history_end(), 1);
    rig.stored(0, a);
}

void native_order() {
    Rig rig;
    no::WorkingRequestCore core(no::RunIdentity{"issue-972", 2});
    no::bind_journal_codec(core, rig.store);
    const auto one = native(1), five = native(5), three = native(3);
    SwitchSource input(one, five);
    const auto status = no::append_journal_record(core, input);
    input.compared();
    check("valid staged ordinal", status, Status::ok);
    check("one stored ordinal", core.history_end(), 1);
    rig.stored(0, five);
    const auto before = rig.digest();
    const auto remembered = core.last_ordinal();
    const auto next = no::append_journal_record(core, pd::ScratchSpanSource(bytes(three)));
    check("ordinal 3 after stored ordinal 5", next, Status::corrupt);
    check("remembered stored ordinal", remembered, 5);
    check("no history row for ordinal 3", core.history_end(), 1);
    check("refusal preserved ordinal", core.last_ordinal(), 5);
    check("refusal preserved ordinal 5 record", rig.digest() == before, true);

    // A itself is eligible; only the copy in the transaction goes backwards.
    const auto six = native(6);
    SwitchSource backwards(six, three);
    const auto stale = no::append_journal_record(core, backwards);
    backwards.compared();
    check("staged ordinal 3 after 5", stale, Status::corrupt);
    check("staged refusal preserved history", core.history_end(), 1);
    check("staged refusal preserved ordinal", core.last_ordinal(), 5);
    check("staged refusal preserved store", rig.digest() == before, true);
}

void admission_key(bool invalid) {
    Rig rig;
    ad::Journal journal;
    ad::bind_journal_codec(journal, rig.store);
    need("allocated sequence", journal.next_sequence(), 1);
    // Both 1 and 2 are outstanding: a different but eligible ID is still the wrong key.
    need("second allocation", journal.next_sequence(), 2);
    // A pending abandonment joins the same transaction. Refusal must undo its put too.
    journal.abandon(journal.next_sequence());
    const auto a = admission(1, ad::Outcome::RejectedAffordability);
    auto b = invalid ? a : admission(2, ad::Outcome::RejectedAffordability);
    if (invalid) std::fill_n(b.begin(), 8, std::byte{0xff}); // Invalid field count.
    const auto before = rig.digest();
    SwitchSource input(a, b);
    const auto status = ad::append_journal_record(journal, input);
    input.compared();
    check(invalid ? "admission invalid staged record" : "admission staged sequence differs from key",
          status, Status::corrupt);
    check("admission rollback closed transaction", rig.store.transaction_active(), false);
    check("admission rollback restored store", rig.digest() == before, true);
    check("no pending record or abandonment marker leaked", rig.store.record_count(), 0);
    check("no failed admission row", journal.event_count(), 0);
    check("no failed rejection published", journal.last_rejected_command_bar(), -1);
    check("retry allocated sequence 1", ad::append_journal_record(journal, pd::ScratchSpanSource(bytes(a))),
          Status::ok);
    rig.stored(1, a);
    check("retry published record and pending abandonment", rig.store.record_count(), 2);
    check("stored rejection at bar 5", journal.last_rejected_command_bar(), 5);
    check("second allocation still outstanding", journal.has_unfinished_events(), true);
    journal.abandon(2);
    check("only one allocation consumed", journal.has_unfinished_events(), false);
}

void admission_gate() {
    Rig rig;
    ad::Journal journal;
    ad::bind_journal_codec(journal, rig.store);
    need("allocated sequence", journal.next_sequence(), 1);
    const auto rejected = admission(1, ad::Outcome::RejectedAffordability);
    const auto admitted = admission(1, ad::Outcome::Admitted);
    SwitchSource input(rejected, admitted);
    const auto status = ad::append_journal_record(journal, input);
    input.compared();
    check("valid changed admission outcome", status, Status::ok);
    rig.stored(1, admitted);
    const auto gate = journal.last_rejected_command_bar();
    const auto folded = journal.folded_window();
    check("one folded command", folded.count, 1);
    check("folded stored sequence", folded.last_sequence, 1);
    check("folded admitted command", folded.last_rejected_command_bar, -1);
    check("gate before fold", gate, -1);
    check("gate after fold", journal.last_rejected_command_bar(), -1);
}

void controls() {
    const pd::ScratchSpanSource empty({nullptr, 0});
    {
        Rig rig;
        no::WorkingRequestCore core(no::RunIdentity{"issue-972", 3});
        no::bind_journal_codec(core, rig.store);
        const auto one = native(1);
        check("immutable native control", no::append_journal_record(core,
              pd::ScratchSpanSource(bytes(one))), Status::ok);
        const auto before = rig.digest();
        check("empty native input", no::append_journal_record(core, empty), Status::corrupt);
        check("empty input preserved native store", rig.digest() == before, true);
        check("empty input preserved ordinal", core.last_ordinal(), 1);
        rig.stored(0, one);
    }
    Rig rig;
    ad::Journal journal;
    ad::bind_journal_codec(journal, rig.store);
    need("control allocation", journal.next_sequence(), 1);
    const auto before = rig.digest();
    check("empty admission input", ad::append_journal_record(journal, empty), Status::corrupt);
    const auto wrong = admission(2, ad::Outcome::Admitted);
    check("unallocated immutable sequence", ad::append_journal_record(journal,
          pd::ScratchSpanSource(bytes(wrong))), Status::programming_error);
    check("controls preserved store", rig.digest() == before, true);
    check("controls preserved event count", journal.event_count(), 0);
    check("controls preserved allocation", journal.has_unfinished_events(), true);
    const auto right = admission(1, ad::Outcome::Admitted);
    check("allocated immutable sequence", ad::append_journal_record(journal,
          pd::ScratchSpanSource(bytes(right))), Status::ok);
    rig.stored(1, right);
    check("control allocation consumed", journal.has_unfinished_events(), false);
}
} // namespace

int main() {
    struct Case { const char* name; void (*run)(); };
    const Case cases[] = {
        {"native invalid variant", invalid_native}, {"native ordinal", native_order},
        {"admission sequence key", [] { admission_key(false); }},
        {"admission invalid value", [] { admission_key(true); }},
        {"admission rejection gate", admission_gate}, {"empty and wrong inputs", controls}
    };
    int failures = 0;
    for (const auto& row : cases) {
        try { row.run(); }
        catch (const Stop&) { ++failures; }
        catch (const std::exception& error) {
            std::fprintf(stderr, "NOT COMPARED: %s: actual=%s expected=no exception\n",
                         row.name, error.what());
            ++failures;
        }
    }
    if (failures) return 1;
    std::puts("PASS: stored records agree with native ordinals, admission keys and rejection gates");
    return 0;
}
