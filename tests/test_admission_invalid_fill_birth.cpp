#include "command_scratch_cache_run_fixture.hpp"
#include "../src/source/admission_journal_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {
namespace ad = pineforge::admission;
namespace pd = pineforge::detail;
namespace fixture = command_cache_test;
using Status = pd::ScratchStatus;
using Bytes = std::vector<std::byte>;

std::ostream& operator<<(std::ostream& out, Status status) {
    if (status == Status::ok) return out << "ok";
    if (status == Status::corrupt) return out << "corrupt";
    return out << "ScratchStatus(" << static_cast<unsigned>(status) << ')';
}

template<class T, class U>
void check(const char* verdict, std::string_view row, const T& actual, const U& expected) {
    if (actual == expected) return;
    std::cerr << verdict << ' ' << row << ": expected=" << expected << " actual=" << actual << '\n';
    std::exit(EXIT_FAILURE);
}

ad::Event command(std::uint64_t sequence, pineforge::OrderBirth birth) {
    auto input = std::make_shared<ad::CommandObservation>();
    input->command = sequence;
    input->bar = birth.bar();
    input->birth = birth;
    return ad::CommandEvent{std::move(input), ad::Outcome::Admitted, sequence, {}, {}};
}

const ad::CommandObservation& observation(const ad::Event& event) {
    const auto* cmd = std::get_if<ad::CommandEvent>(&event);
    check("NOT COMPARED", "command event kind", event.index(), 0U);
    check("NOT COMPARED", "command observation present", bool(cmd->observation), true);
    return *cmd->observation;
}

struct Mutation {
    const char* field;
    std::uint64_t tag, original, replacement;
};

// Match the length-prefixed path, type tag and original scalar together.
// Replace only the final eight bytes of that unique row in a fresh copy.
Bytes mutate(Bytes bytes, const Mutation& m) {
    const std::string path = std::string("event.observation.birth.") + m.field;
    Bytes row;
    fixture::append_word(row, path.size());
    for (unsigned char c : path) row.push_back(static_cast<std::byte>(c));
    fixture::append_word(row, m.tag);
    fixture::append_word(row, m.original);
    const auto at = std::search(bytes.begin(), bytes.end(), row.begin(), row.end());
    check("NOT COMPARED", path + " with expected tag and original word found", at != bytes.end(), true);
    check("NOT COMPARED", path + " unique", std::search(at + 1, bytes.end(), row.begin(), row.end())
          == bytes.end(), true);
    Bytes replacement;
    fixture::append_word(replacement, m.replacement);
    std::copy(replacement.begin(), replacement.end(), at + (row.size() - replacement.size()));
    return bytes;
}

void run() {
    using namespace pineforge;
    std::array<std::byte, fixture::page_bytes> page{};
    const pd::ScratchBuffer buffer{page.data(), page.size()};
    Bytes storage(256 * page.size());
    pd::FixedMemoryScratchBacking backing{fixture::buffer(storage), page.size()};
    pd::CommandScratchStore store{fixture::config(), &backing};
    check("NOT COMPARED", "scratch store open", store.status(), Status::ok);
    ad::Journal journal;
    ad::bind_journal_codec(journal, store);

    // Zero first_fill is normal for a direct command. Keep it as a real
    // committed record so an empty journal cannot stand in for success.
    const auto direct = command(journal.next_sequence(), OrderBirth::direct_command(6, 100));
    check("NOT COMPARED", "direct command append", ad::append_journal_record(journal,
          ad::encode_journal_record(direct)), Status::ok);
    const auto before = journal.folded_window();
    check("NOT COMPARED", "initial fold count", before.count, 1U);
    check("NOT COMPARED", "initial fold sequence", before.last_sequence, 1U);

    const auto sequence = journal.next_sequence();
    check("NOT COMPARED", "allocated fill command sequence", sequence, 2U);
    const auto event = command(sequence, OrderBirth::fill_evaluation(7, 101,
        BirthCursor::segment(BirthCursorDomain::HistoricalPath, 2, 4), 12.5, 13, 15, 17));
    const auto encoded = ad::encode_journal_record(event);
    check("NOT COMPARED", "nonempty encoded fill command", encoded.size() > 0, true);
    Bytes valid(encoded.size());
    check("NOT COMPARED", "read encoded fill command", encoded.read(0, fixture::buffer(valid)), Status::ok);
    const pd::ScratchSpanSource original{fixture::bytes(valid)};
    ad::Event decoded;
    check("NOT COMPARED", "owning decode of original", ad::decode_journal_record(original, buffer, decoded),
          Status::ok);
    check("NOT COMPARED", "original command sequence", observation(decoded).command, sequence);
    check("NOT COMPARED", "original is a fill evaluation", observation(decoded).birth.from_fill(), true);
    check("NOT COMPARED", "original first_fill", observation(decoded).birth.first_fill(), 13U);

    const pd::ScratchSpanSource empty{{nullptr, 0}};
    check("NOT COMPARED", "empty record control", ad::append_journal_record(journal, empty), Status::corrupt);
    const Mutation cases[] = {
        {"first_fill", 0, 13, 0}, // #968
        {"cursor_domain", 1, 1, 0}, {"cursor_count", 1, 4, 3},
        {"cursor_index", 1, 2, 3}, // a segment cannot start at the last point
        {"bar", 1, 7, UINT64_MAX}, {"cursor_price", 2, 0x4029000000000000ULL, 0x7ff0000000000000ULL},
        {"last_fill", 0, 15, 12}, {"evaluation_ordinal", 0, 17, 0}
    };
    for (const auto& m : cases) {
        const auto bytes = mutate(valid, m);
        const pd::ScratchSpanSource bad{fixture::bytes(bytes)};
        const auto row = std::string(m.field) + "=" + std::to_string(m.replacement);
        check("NOT COMPARED", row + " owning decode", ad::decode_journal_record(bad, buffer, decoded),
              Status::corrupt);
        check("FAIL", row + " append", ad::append_journal_record(journal, bad), Status::corrupt);
        check("FAIL", row + " stored records", store.record_count(), 1U);
        check("FAIL", row + " journal count", journal.event_count(), 1U);
        check("FAIL", row + " allocation outstanding", journal.has_unfinished_events(), true);
        auto digest = before.digest;
        ad::JournalRecordSummary summary{before.last_sequence, -1, false};
        check("FAIL", row + " fold", ad::fold_validated_journal_record(bad, buffer, summary,
              digest, before.last_sequence), Status::corrupt);
        check("FAIL", row + " fold digest", digest, before.digest);
        check("FAIL", row + " fold sequence", summary.sequence, before.last_sequence);
    }
    check("FAIL", "fold after refusals", journal.folded_window().digest, before.digest);
    check("FAIL", "valid retry on the same sequence", ad::append_journal_record(journal, original), Status::ok);
    check("FAIL", "allocation consumed by retry", journal.has_unfinished_events(), false);
    const auto fold = journal.folded_window();
    check("FAIL", "fold count after retry", fold.count, 2U);
    check("FAIL", "fold sequence after retry", fold.last_sequence, sequence);
    const auto& events = journal.events();
    check("FAIL", "stored events read back", events.size(), 2U);
    check("FAIL", "earlier command sequence", observation(events.front()).command, 1U);
    check("FAIL", "earlier direct command", observation(events.front()).birth.cause()
          == OrderBirthCause::DirectCommand, true);
    check("FAIL", "earlier first_fill", observation(events.front()).birth.first_fill(), 0U);
    check("FAIL", "retry command sequence", observation(events.back()).command, sequence);
    check("FAIL", "retry first_fill", observation(events.back()).birth.first_fill(), 13U);
    check("FAIL", "retry last_fill", observation(events.back()).birth.last_fill(), 15U);
}
} // namespace

int main() {
    try { run(); }
    catch (const std::exception& e) {
        std::cerr << "NOT COMPARED journal operation: expected=normal return actual=" << e.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "PASS invalid fill births refused; original command folded and read back\n";
}
