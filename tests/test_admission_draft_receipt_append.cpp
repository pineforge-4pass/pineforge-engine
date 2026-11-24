#include "command_scratch_cache_run_fixture.hpp"
#include <pineforge/run_failure.hpp>
#include <pineforge/source/market_admission.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace ad = pineforge::admission;
namespace pd = pineforge::detail;
using Status = pd::ScratchStatus;

std::ostream& operator<<(std::ostream& out, Status status) {
    if (status == Status::ok) return out << "ok";
    if (status == Status::corrupt) return out << "corrupt";
    return out << static_cast<unsigned>(status);
}
struct Mismatch {};
template<class Expected, class Actual>
void check(const char* verdict, const std::string& row, Expected expected, Actual actual) {
    if (expected == actual) return;
    std::cerr << verdict << ' ' << row << ": expected=" << expected << " actual=" << actual << '\n';
    throw Mismatch{};
}

std::string wire(const ad::Event& event) {
    const auto encoded = ad::encode_journal_record(event);
    check("NOT COMPARED", "nonempty encoded event", true, encoded.size() > 0);
    std::string bytes(encoded.size(), '\0');
    check("NOT COMPARED", "read encoded event", Status::ok, encoded.read(0,
          {reinterpret_cast<std::byte*>(bytes.data()), bytes.size()}));
    return bytes;
}
pd::ScratchSpanSource source(const std::string& bytes) {
    return pd::ScratchSpanSource({reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()});
}
std::string word(std::uint64_t value) {
    std::string bytes;
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<char>(value >> (8 * i)));
    return bytes;
}
struct Mutation {
    const char* path; std::uint64_t tag, before, after;
};
std::string change_word(std::string bytes, const Mutation& m) {
    const std::string path = m.path;
    const auto key = word(path.size()) + path + word(m.tag);
    const auto at = bytes.find(key);
    check("NOT COMPARED", path + " field present", true, at != std::string::npos);
    check("NOT COMPARED", path + " field unique", true, bytes.find(key, at + 1) == std::string::npos);
    const auto pos = at + key.size();
    check("NOT COMPARED", path + " complete word", true, bytes.size() - pos >= 8);
    std::uint64_t original = 0;
    for (unsigned i = 0; i < 8; ++i)
        original |= std::uint64_t(static_cast<unsigned char>(bytes[pos + i])) << (8 * i);
    check("NOT COMPARED", path + " original word", m.before, original);
    check("NOT COMPARED", path + " word changes", true, original != m.after);
    bytes.replace(pos, 8, word(m.after));
    return bytes;
}

void exercise() {
    constexpr auto page_size = command_cache_test::page_bytes;
    std::vector<std::byte> storage(1024 * page_size);
    pd::FixedMemoryScratchBacking backing({storage.data(), storage.size()}, page_size);
    pd::CommandScratchStore store(command_cache_test::config(), &backing);
    check("NOT COMPARED", "open scratch store", Status::ok, store.status());
    ad::Journal journal;
    ad::bind_journal_codec(journal, store);
    const auto command = [&] {
        auto input = std::make_shared<ad::CommandObservation>();
        input->command = journal.next_sequence();
        input->bar = 10;
        ad::CommandEvent event;
        event.observation = input;
        journal.append(event);
        return input;
    };
    command(); // Sequence 1 is an earlier, nonzero receipt sequence in the negative cases.
    const auto sibling = command(), origin = command();
    ad::SizingEvent size;
    size.receipt = {journal.next_sequence(), 11, 10, origin->command};
    journal.append(size);
    const auto prefix = journal.folded_window();
    check("NOT COMPARED", "seeded prefix count", 4u, prefix.count);
    check("NOT COMPARED", "seeded last sequence", 4u, prefix.last_sequence);
    ad::ReviewEvent review;
    review.receipt = {journal.next_sequence(), ad::Checkpoint::ExplicitPair, 10, 0};
    review.book.resize(2); // Sibling with no receipt, followed by an empty draft.
    review.book.front().draft.bind(sibling);
    review.reviewed.resize(1);
    auto per_order = review.receipt;
    per_order.target_command = origin->command;
    auto& draft = review.reviewed.front().draft;
    draft.bind(origin);
    draft.reviewed(per_order);
    draft.sizing_revised(size.receipt);
    const ad::Event valid = review;
    const auto original = wire(valid);
    std::array<std::byte, page_size> page{};
    ad::Event decoded;
    const auto decode = [&](const std::string& bytes) {
        return ad::decode_journal_record(source(bytes), {page.data(), page.size()}, decoded);
    };
    check("NOT COMPARED", "owning decode of original", Status::ok, decode(original));
    check("NOT COMPARED", "decoded batch sequence", 5u, ad::sequence(decoded));

    const auto still_pending = [&](const std::string& row) {
        check("FAIL", row + " stored records", 4u, store.record_count());
        check("FAIL", row + " journal events", 4u, journal.event_count());
        check("FAIL", row + " allocation outstanding", true, journal.has_unfinished_events());
        const auto folded = journal.folded_window();
        check("FAIL", row + " fold count", prefix.count, folded.count);
        check("FAIL", row + " fold digest", prefix.digest, folded.digest);
    };
    const auto refuse = [&](const std::string& row, const std::string& bytes) {
        check("NOT COMPARED", row + " owning decode", Status::corrupt, decode(bytes));
        check("FAIL", row + " append", Status::corrupt, ad::append_journal_record(journal, source(bytes)));
        still_pending(row);
    };
    const Mutation mutations[] = {
        {"event.reviewed[0].draft.review.target_command", 0, 3, 2},
        {"event.reviewed[0].draft.review.sequence", 0, 5, 3},
        {"event.reviewed[0].draft.review.sequence", 0, 5, 1},
        {"event.reviewed[0].draft.review.bar", 1, 10, 9},
        {"event.reviewed[0].draft.review.checkpoint", 1, 1, 99},
        {"event.reviewed[0].draft.sizing_revision.target_command", 0, 3, 2},
        {"event.reviewed[0].draft.sizing_revision.sequence", 0, 4, 3},
        {"event.reviewed[0].draft.sizing_revision.sequence", 0, 4, 1},
        {"event.reviewed[0].draft.sizing_revision.bar", 1, 10, 9},
        {"event.reviewed[0].draft.sizing_revision.cause_fill", 0, 11, 0},
        {"event.book[0].draft.observation.command", 0, 2, 0},
    };
    for (const auto& m : mutations)
        refuse(std::string(m.path) + "=" + std::to_string(m.after), change_word(original, m));
    refuse("empty byte source", {});

    // A moved-from shared_ptr is empty; the optional scalar receipt stays engaged.
    for (bool with_review : {true, false}) {
        const std::string row = with_review ? "moved-from review" : "moved-from sizing revision";
        ad::Draft single;
        single.bind(origin);
        if (with_review) single.reviewed(per_order); else single.sizing_revised(size.receipt);
        ad::Draft destination = std::move(single);
        check("NOT COMPARED", row + " destination has origin", true, destination.observation() == origin);
        check("NOT COMPARED", row + " source has origin", false, bool(single.observation()));
        check("NOT COMPARED", row + " review present", with_review, single.review().has_value());
        check("NOT COMPARED", row + " revision present", !with_review, single.sizing_revision().has_value());
        review.reviewed.front().draft = single;
        const ad::Event orphan = review;
        if (with_review) {
            check("NOT COMPARED", row + " owning decode", Status::corrupt, decode(wire(orphan)));
            bool threw = false;
            try { journal.append(orphan); }
            catch (const pineforge::RunFailureInfo& failure) {
                check("FAIL", row + " refusal code", static_cast<int>(pineforge::RunFailureCode::engine_invariant),
                      static_cast<int>(failure.run_failure_code()));
                threw = true;
            }
            check("FAIL", row + " bound append refused", true, threw);
            still_pending(row);
        } else refuse(row, wire(orphan));
    }

    check("FAIL", "valid retry", Status::ok, ad::append_journal_record(journal, source(original)));
    check("FAIL", "retry consumed allocation", false, journal.has_unfinished_events());
    check("FAIL", "retry stored records", 5u, store.record_count());
    const auto folded = journal.folded_window();
    check("FAIL", "retry fold count", 5u, folded.count);
    check("FAIL", "retry fold sequence", 5u, folded.last_sequence);
    const auto& events = journal.events();
    check("FAIL", "retry readback count", 5u, events.size());
    check("FAIL", "retry readback matches original bytes", true, wire(events.back()) == original);
    const auto* last = std::get_if<ad::ReviewEvent>(&events.back());
    check("FAIL", "retry readback is a review", true, last != nullptr);
    check("FAIL", "retry book rows", 2u, last->book.size());
    const auto& empty = last->book.back().draft;
    check("FAIL", "empty draft has any observation or receipt", false,
          bool(empty.observation() || empty.review() || empty.sizing_revision()));
}
} // namespace

int main() {
    try { exercise(); }
    catch (Mismatch) { return 1; }
    catch (const std::exception& e) {
        std::cerr << "NOT COMPARED journal fixture: expected=completed actual=" << e.what() << '\n';
        return 1;
    }
    std::cout << "PASS nested draft receipts refused before storage; valid retry folds and reads back\n";
}
