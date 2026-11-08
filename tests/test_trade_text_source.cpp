#include <pineforge/trade_text_source.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
using namespace pineforge;
using namespace pineforge::report;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Error, class F>
void throws(F operation) {
    try { operation(); }
    catch (const Error&) { return; }
    throw std::runtime_error("expected exception was absent");
}

void check_source() {
    TradeLedger ledger;
    Trade row{};
    row.entry_id = "L";
    row.exit_id = "close";
    row.entry_comment = std::string("a\0b", 3);
    row.exit_comment = "abcdefghijklmnopq";
    row.entry_time = 100;
    row.exit_time = 200;
    row.entry_incarnation = 7;
    ledger.push_back(row);
    LedgerTradeSourceV1 source(ledger);

    // One appended row has logical index zero and the literal stored provenance.
    require(source.size() == 1 && source.at(0).entry_incarnation == 7,
            "ledger row and identity");
    // L, close, a/NUL/b and a through q contain 1, 5, 3 and 17 bytes.
    require(source.text_size(0, TradeTextV1::EntryId) == 1
                && source.text_size(0, TradeTextV1::ExitId) == 5
                && source.text_size(0, TradeTextV1::EntryComment) == 3
                && source.text_size(0, TradeTextV1::ExitComment) == 17,
            "all four stored byte lengths");
    std::array<char, 5> bytes{{'?', '?', '?', '?', '?'}};
    // The first three bytes are a/NUL/b; a bounded copy adds no terminator.
    require(source.read_text(0, TradeTextV1::EntryComment, 0, bytes.data(), 3) == 3
                && std::memcmp(bytes.data(), "a\0b", 3) == 0 && bytes[3] == '?',
            "embedded NUL survives");
    std::string whole;
    // Seventeen bytes transferred four at a time give four full chunks and one byte.
    const std::array<std::size_t, 5> lengths{{4, 4, 4, 4, 1}};
    std::uint64_t offset = 0;
    for (const auto length : lengths) {
        const auto copied = source.read_text(0, TradeTextV1::ExitComment,
                                             offset, bytes.data(), 4);
        require(copied == length, "bounded chunk length");
        whole.append(bytes.data(), copied);
        offset += copied;
    }
    // Concatenating a through q recovers the complete literal without a suffix loss.
    require(whole == "abcdefghijklmnopq", "complete multi-read text");
    // Offset 17 is the end of the seventeen-byte field.
    require(source.read_text(0, TradeTextV1::ExitComment, 17, nullptr, 0) == 0,
            "empty read at end");
    throws<std::out_of_range>([&] { source.text_size(1, TradeTextV1::EntryId); });
    throws<std::out_of_range>([&] {
        source.read_text(1, TradeTextV1::EntryId, 0, bytes.data(), 1);
    });
    throws<std::invalid_argument>([&] {
        source.text_size(0, static_cast<TradeTextV1>(99));
    });
    throws<std::invalid_argument>([&] {
        source.read_text(0, static_cast<TradeTextV1>(99), 0, bytes.data(), 1);
    });
    throws<std::out_of_range>([&] {
        source.read_text(0, TradeTextV1::ExitComment, 18, bytes.data(), 1);
    });
    throws<std::invalid_argument>([&] {
        source.read_text(0, TradeTextV1::EntryId, 0, nullptr, 1);
    });
    std::array<TradeRowV1, 2> page{};
    // One source row fits in a two-row page; the second slot remains zero.
    require(source.read_page(0, page.data(), 2) == 1
                && page[0].value.entry_time == 100 && page[1].value.entry_time == 0,
            "detached numeric page");
    // A false callback consumes the sole row but does not complete the visit.
    const auto stopped = source.visit_backward(0, 1, nullptr,
        [](void*, LogicalIndexV1 index, TradeRowV1 value) {
            require(index == 0 && value.value.exit_time == 200, "reverse visit value");
            return false;
        });
    require(stopped.visited == 1 && !stopped.complete, "visitor stop semantics");
    throws<std::invalid_argument>([&] { source.visit(1, 0, nullptr, nullptr); });
}
}

int main() {
    try { check_source(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL trade text source: %s\n", error.what());
        return 1;
    }
    std::puts("PASS trade text source");
    return 0;
}
