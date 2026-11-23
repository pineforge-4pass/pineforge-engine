#include "command_scratch_cache_run_fixture.hpp"
#include "../src/source/admission_journal_codec.hpp"

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
namespace ad = pineforge::admission;
namespace pd = pineforge::detail;
namespace fixture = command_cache_test;
using Position = pineforge::BirthCursorPosition;
using Status = pd::ScratchStatus;
using Bytes = std::vector<std::byte>;

struct CheckFailed {};

void equal(const char* verdict, const char* what, std::uint64_t actual,
           std::uint64_t expected) {
    if (actual == expected) return;
    std::fprintf(stderr, "%s %s: expected=%llu actual=%llu\n", verdict, what,
                 static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual));
    throw CheckFailed{};
}

void equal(const char* verdict, const char* what, Status actual, Status expected) {
    equal(verdict, what, static_cast<unsigned>(actual), static_cast<unsigned>(expected));
}

Bytes encode_fill(pineforge::BirthCursor cursor) {
    auto observation = std::make_shared<ad::CommandObservation>();
    observation->command = 1;
    observation->birth = pineforge::OrderBirth::fill_evaluation(7, 101, cursor, 12.5, 13, 15, 17);
    const ad::Event event = ad::CommandEvent{observation, ad::Outcome::Admitted, 1, {}, {}};
    const auto source = ad::encode_journal_record(event);
    equal("NOT COMPARED", "encoded fill is nonempty", source.size() != 0, true);
    Bytes wire(source.size());
    equal("NOT COMPARED", "read encoded fill", source.read(0, fixture::buffer(wire)), Status::ok);
    return wire;
}

void check_case(unsigned raw, Position legal, const Bytes& valid, std::size_t offset) {
    Bytes storage(256 * 1024);
    pd::FixedMemoryScratchBacking backing(fixture::buffer(storage), fixture::page_bytes);
    pd::CommandScratchStore store(fixture::config(), &backing);
    equal("NOT COMPARED", "open scratch store", store.status(), Status::ok);
    ad::Journal journal;
    ad::bind_journal_codec(journal, store);
    equal("NOT COMPARED", "reserved command sequence", journal.next_sequence(), 1);
    std::array<std::byte, fixture::page_bytes> page{};
    const pd::ScratchBuffer buffer{page.data(), page.size()};

    // Keep another event kind in the destination to detect a partial decode.
    ad::SizingEvent marker;
    marker.receipt.sequence = 91;
    ad::Event decoded = marker;
    auto wire = valid;
    wire[offset] = static_cast<std::byte>(raw);
    const pd::ScratchSpanSource bad(fixture::bytes(wire));
    ad::JournalRecordSummary summary{91, -17, true};
    constexpr std::uint64_t seed = 12345;
    auto digest = seed;
    const auto append = ad::append_journal_record(journal, bad);
    const auto decode = ad::decode_journal_record(bad, buffer, decoded);
    const auto fold = ad::fold_validated_journal_record(bad, buffer, summary, digest);
    if (append != Status::corrupt || decode != Status::corrupt || fold != Status::corrupt) {
        std::fprintf(stderr, "FAIL cursor_position=%u: append=%u decode=%u fold=%u expected=%u (corrupt)\n",
                     raw, static_cast<unsigned>(append), static_cast<unsigned>(decode),
                     static_cast<unsigned>(fold), static_cast<unsigned>(Status::corrupt));
        throw CheckFailed{};
    }
    equal("FAIL", "records stored after rejection", store.record_count(), 0);
    equal("FAIL", "allocation still outstanding", journal.has_unfinished_events(), true);
    const auto* retained = std::get_if<ad::SizingEvent>(&decoded);
    equal("FAIL", "owning output still holds marker", retained != nullptr, true);
    equal("FAIL", "marker sequence", retained->receipt.sequence, 91);
    equal("FAIL", "fold summary sequence after rejection", summary.sequence, 91);
    equal("FAIL", "fold digest after rejection", digest, seed);

    const pd::ScratchSpanSource good(fixture::bytes(valid));
    equal("FAIL", "append valid cursor on the same allocation",
          ad::append_journal_record(journal, good), Status::ok);
    equal("FAIL", "allocation consumed", journal.has_unfinished_events(), false);
    const auto& events = journal.events();
    equal("FAIL", "stored event count", events.size(), 1);
    const auto* stored = std::get_if<ad::CommandEvent>(&events.front());
    equal("FAIL", "stored command has an observation", stored && stored->observation, true);
    equal("FAIL", "stored command sequence", stored->observation->command, 1);
    equal("FAIL", "stored fill birth", stored->observation->birth.from_fill(), true);
    const auto position = stored->observation->birth.cursor().position();
    equal("FAIL", "stored cursor position", static_cast<unsigned>(position), static_cast<unsigned>(legal));

    ad::Journal reencoded;
    equal("NOT COMPARED", "round-trip command sequence", reencoded.next_sequence(), 1);
    reencoded.append(events.front());
    const auto stored_fold = journal.folded_window();
    const auto round_trip = reencoded.folded_window();
    equal("FAIL", "stored fold count", stored_fold.count, 1);
    equal("FAIL", "round-trip fold count", round_trip.count, 1);
    equal("FAIL", "stored digest advanced", stored_fold.digest != ad::Journal::Prefix{}.digest, true);
    equal("FAIL", "fold digest of re-encoded event", round_trip.digest, stored_fold.digest);
}
} // namespace

int main() {
    try {
        using pineforge::BirthCursor;
        constexpr auto domain = pineforge::BirthCursorDomain::HistoricalPath;
        const Bytes point = encode_fill(BirthCursor::point(domain, 1, 4));
        const Bytes segment = encode_fill(BirthCursor::segment(domain, 1, 4));
        equal("NOT COMPARED", "point/segment record lengths", segment.size(), point.size());
        // These factory records differ only in the low byte of cursor_position.
        const auto at = std::mismatch(point.begin(), point.end(), segment.begin()).first;
        equal("NOT COMPARED", "position byte present", at != point.end(), true);
        const auto offset = static_cast<std::size_t>(at - point.begin());
        equal("NOT COMPARED", "point position byte", std::to_integer<unsigned>(*at), 1);
        equal("NOT COMPARED", "segment position byte", std::to_integer<unsigned>(segment[offset]), 2);
        equal("NOT COMPARED", "all bytes after position match",
              std::equal(at + 1, point.end(), segment.begin() + offset + 1), true);

        check_case(7, Position::Segment, segment, offset);
        check_case(static_cast<unsigned>(Position::None), Position::Point, point, offset);
    } catch (const CheckFailed&) {
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NOT COMPARED cursor position: expected=completed comparison actual=%s\n",
                     error.what());
        return 1;
    }
    std::puts("PASS rejected fill cursor positions 7 and None; Segment and Point round-tripped");
    return 0;
}
