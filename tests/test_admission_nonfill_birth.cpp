#include "command_scratch_cache_run_fixture.hpp"
#include "../src/source/admission_journal_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
namespace ad = pineforge::admission;
namespace pd = pineforge::detail;
namespace fixture = command_cache_test;
using Bytes = std::vector<std::byte>;
using Status = pd::ScratchStatus;
using pineforge::OrderBirth;

struct Mismatch {};
template<class T, class U>
void check(const char* verdict, const std::string& row, const T& expected, const U& actual) {
    if (expected == actual) return;
    std::cerr << verdict << ' ' << row << ": expected " << expected << ", got " << actual << '\n';
    throw Mismatch{};
}
std::string status(Status s) {
    if (s == Status::ok) return "ok";
    if (s == Status::corrupt) return "corrupt";
    return std::to_string(static_cast<unsigned>(s));
}
std::uint64_t bits(double value) {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}
Bytes encode(const ad::Event& event) {
    const auto source = ad::encode_journal_record(event);
    check("NOT COMPARED", "encoder produced a nonempty record", true, source.size() != 0);
    Bytes wire(source.size());
    check("NOT COMPARED", "read encoded record", "ok", status(source.read(0, fixture::buffer(wire))));
    return wire;
}

// Match a complete wire row, including the type and the original value.
// A missing, stale, or ambiguous field must never count as a corruption case.
std::size_t value_offset(const Bytes& wire, const std::string& path,
                         std::uint64_t tag, std::uint64_t value) {
    Bytes row;
    fixture::append_word(row, path.size());
    for (char c : path) row.push_back(static_cast<std::byte>(c));
    fixture::append_word(row, tag);
    fixture::append_word(row, value);
    const auto at = std::search(wire.begin(), wire.end(), row.begin(), row.end());
    std::size_t matches = 0;
    for (auto it = at; it != wire.end(); it = std::search(it + 1, wire.end(), row.begin(), row.end()))
        ++matches;
    check("NOT COMPARED", path + " matching rows (word " + std::to_string(value) + ")", 1u, matches);
    return static_cast<std::size_t>(at - wire.begin()) + row.size() - 8;
}
struct Change { const char* field; std::uint64_t tag, from, to; };
void change(Bytes& wire, const std::string& birth, const Change& c) {
    const auto path = birth + "." + c.field;
    const auto at = value_offset(wire, path, c.tag, c.from);
    if (c.from == c.to) {
        std::cerr << "NOT COMPARED " << path << ": original " << c.from
                  << ", replacement " << c.to << " must differ\n";
        throw Mismatch{};
    }
    Bytes replacement;
    fixture::append_word(replacement, c.to);
    std::copy(replacement.begin(), replacement.end(), wire.begin() + at);
}

void compare_births() {
    Bytes medium(256 * fixture::page_bytes);
    pd::FixedMemoryScratchBacking backing({medium.data(), medium.size()}, fixture::page_bytes);
    pd::CommandScratchStore store(fixture::config(), &backing);
    check("NOT COMPARED", "open memory store", "ok", status(store.status()));
    ad::Journal journal;
    ad::bind_journal_codec(journal, store);
    auto observation = std::make_shared<ad::CommandObservation>();
    observation->command = journal.next_sequence();
    observation->id = "issue-971";
    observation->birth = OrderBirth::direct_command(12, 12000);
    ad::CommandEvent command;
    command.observation = observation;
    command.before.resize(3);
    command.before[0].birth = OrderBirth::chart_evaluation(11, 11000);
    command.before[1].birth = OrderBirth{};
    command.before[2].birth = OrderBirth::fill_evaluation(10, 10000,
        pineforge::BirthCursor::point(pineforge::BirthCursorDomain::HistoricalPath, 1, 4),
        1.5, 7, 9, 2);
    const ad::Event input = command;
    const auto wire = encode(input);
    const std::string direct = "event.observation.birth", chart = "event.before[0].birth",
                      unattributed = "event.before[1].birth", fill = "event.before[2].birth";
    value_offset(wire, "event.observation.command", 0, 1);
    value_offset(wire, direct + ".cause", 1, 1);
    value_offset(wire, chart + ".cause", 1, 2);
    value_offset(wire, unattributed + ".cause", 1, 0);
    value_offset(wire, fill + ".cause", 1, 3);
    value_offset(wire, fill + ".first_fill", 0, 7);
    value_offset(wire, fill + ".cursor_price", 2, bits(1.5));

    std::array<std::byte, fixture::page_bytes> storage{};
    const pd::ScratchBuffer page{storage.data(), storage.size()};
    const pd::ScratchSpanSource valid(fixture::bytes(wire));
    ad::Event control;
    check("NOT COMPARED", "decode canonical control", "ok",
          status(ad::decode_journal_record(valid, page, control)));
    check("NOT COMPARED", "canonical control round-trips byte for byte", true, encode(control) == wire);
    const auto refused = [&](const std::string& row, const Bytes& bytes) {
        const pd::ScratchSpanSource source(fixture::bytes(bytes));
        ad::SizingEvent marker;
        marker.receipt.sequence = 971;
        ad::Event output = marker;
        ad::JournalRecordSummary summary{4321, -31, true};
        std::uint64_t digest = 971;
        const auto append = ad::append_journal_record(journal, source);
        const auto decode = ad::decode_journal_record(source, page, output);
        const auto fold = ad::fold_validated_journal_record(source, page, summary, digest);
        check("FAIL", row + " append/decode/fold", "corrupt/corrupt/corrupt",
              status(append) + "/" + status(decode) + "/" + status(fold));
        const auto* kept = std::get_if<ad::SizingEvent>(&output);
        check("FAIL", row + " decode output", marker.receipt.sequence, kept ? kept->receipt.sequence : 0);
        check("FAIL", row + " fold summary sequence", std::uint64_t{4321}, summary.sequence);
        check("FAIL", row + " digest", std::uint64_t{971}, digest);
        check("FAIL", row + " stored records", 0u, store.record_count());
        check("FAIL", row + " outstanding sequence", true, journal.has_unfinished_events());
    };
    const auto absent = bits(OrderBirth{}.cursor_price());
    const Change first{"first_fill", 0, 0, 7}, price{"cursor_price", 2, absent, bits(1.5)};
    auto example = wire;
    change(example, direct, first);
    change(example, direct, price);
    refused("DirectCommand first_fill=7 cursor_price=1.5", example);

    const Change unused[] = {
        {"cursor_domain", 1, 0, 1}, {"cursor_position", 1, 0, 1},
        {"cursor_index", 1, UINT64_MAX, 0}, {"cursor_count", 1, 0, 4},
        price, {"cursor_price", 2, absent, absent ^ 1},
        {"cursor_price", 2, absent, absent ^ (UINT64_C(1) << 63)},
        first, {"last_fill", 0, 0, 9}, {"evaluation_ordinal", 0, 0, 2},
    };
    const auto single = [&](const std::string& birth, const Change& c) {
        auto changed = wire;
        change(changed, birth, c);
        refused(birth + "." + c.field + "=" + std::to_string(c.to), changed);
    };
    for (const auto& birth : {direct, chart, unattributed})
        for (const auto& c : unused) single(birth, c);
    single(unattributed, {"bar", 1, UINT64_MAX, 12});
    single(unattributed, {"timestamp", 1, 0, 12000});
    refused("empty record (framing control)", {});

    check("FAIL", "canonical retry on the same sequence", "ok",
          status(ad::append_journal_record(journal, valid)));
    check("FAIL", "canonical retry consumed the sequence", false, journal.has_unfinished_events());
    const auto& stored = journal.events();
    check("FAIL", "stored event count", 1u, stored.size());
    check("FAIL", "stored event re-encodes to original bytes", true, encode(stored.front()) == wire);
    auto digest = ad::Journal::Prefix{}.digest;
    check("FAIL", "fold canonical wire", "ok", status(ad::fold_journal_record(valid, page, digest)));
    const auto window = journal.folded_window();
    check("FAIL", "folded event count", 1u, window.count);
    check("FAIL", "folded sequence", observation->command, window.last_sequence);
    check("FAIL", "folded stored event matches canonical wire", digest, window.digest);
}
} // namespace

int main() {
    try { compare_births(); }
    catch (const Mismatch&) { return 1; }
    catch (const std::exception& e) {
        std::cerr << "NOT COMPARED #971 fixture: expected normal completion, got " << e.what() << '\n';
        return 1;
    }
    std::cout << "PASS #971: non-fill birth refusals and canonical retry compared\n";
    return 0;
}
