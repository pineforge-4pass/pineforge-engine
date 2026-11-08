#include "../src/trade_text_writer.hpp"
#include <pineforge/trade_text_source.hpp>

#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
using namespace pineforge;
using namespace pineforge::report;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void check_wire() {
    TradeLedger ledger;
    Trade trade{};
    trade.entry_id = "L";
    trade.exit_id = "close";
    trade.entry_comment = "opened";
    trade.exit_comment = "digits 3678.7059999999997";
    trade.entry_incarnation = 7;
    ledger.push_back(trade);
    LedgerTradeSourceV1 source(ledger);
    std::ostringstream output;
    write_trade_text_companion(output, "run", source, 4);
    // One row retains its zero-based index, incarnation and all literal text.
    const std::string prefix =
        "{\"schema\":\"pf-trade-text\",\"version\":1,\"run_id\":\"run\","
        "\"fields\":[\"run_id\",\"row_index\",\"entry_incarnation\",\"entry_id\","
        "\"exit_id\",\"entry_comment\",\"exit_comment\"]}\n"
        "[\"T\",\"run\",0,7,\"L\",\"close\",\"opened\",\"digits 3678.7059999999997\"]\n"
        "{\"trailer\":\"pf-trade-text\",\"version\":1,\"counts\":{\"T\":1},\"sha256\":\"";
    require(output.str().compare(0, prefix.size(), prefix) == 0,
            "literal text and binding bytes");
    const auto tail = output.str().substr(prefix.size());
    // A SHA-256 token is 64 lowercase hex bytes followed by quote, brace and LF.
    require(tail.size() == 67 && tail.substr(64) == "\"}\n"
                && tail.substr(0, 64).find_first_not_of("0123456789abcdef") == std::string::npos,
            "count and digest trailer grammar");
}

void check_utf8() {
    TradeLedger ledger;
    Trade trade{};
    trade.entry_id = std::string("A\0\"\\\n", 5);
    trade.exit_id = "\xc3\xa9\xf0\x9f\x98\x80";
    trade.entry_comment = "\xe0\x80\x80";
    trade.exit_comment = "\xc3(";
    ledger.push_back(trade);
    LedgerTradeSourceV1 source(ledger);
    std::ostringstream output;
    write_trade_text_companion(output, "utf8", source, 1);
    // Controls use lowercase escapes; split valid scalars survive; each invalid byte is replaced.
    const std::string row =
        "[\"T\",\"utf8\",0,0,\"A\\u0000\\\"\\\\\\u000a\","
        "\"\xc3\xa9\xf0\x9f\x98\x80\","
        "\"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\","
        "\"\xef\xbf\xbd(\"]\n";
    require(output.str().find(row) != std::string::npos,
            "sink string rules across single-byte transfers");
    char raw[3]{};
    // The provider returns the original three invalid bytes without replacement.
    require(source.read_text(0, TradeTextV1::EntryComment, 0, raw, 3) == 3
                && std::string(raw, 3) == "\xe0\x80\x80",
            "raw provider bytes remain exact");
}

void check_large_text() {
    TradeLedger ledger;
    Trade trade{};
    trade.entry_comment.assign(1048577, 'x');
    ledger.push_back(trade);
    LedgerTradeSourceV1 source(ledger);
    std::ostringstream output;
    write_trade_text_companion(output, "large", source, 65536);
    const auto wire = output.str();
    const std::string start = "[\"T\",\"large\",0,0,\"\",\"\",\"";
    const auto first = wire.find(start);
    require(first != std::string::npos, "large row present");
    const auto begin = first + start.size();
    const auto end = wire.find('"', begin);
    // Sixteen 65536-byte transfers plus one byte exceed a 1 MiB line by one payload byte.
    require(end != std::string::npos && end - begin == 1048577,
            "large field has no line cap");
    for (auto index = begin; index < end; ++index)
        require(wire[index] == 'x', "every large field byte survives");
    // The empty exit comment closes the same record, followed by a one-row trailer.
    require(wire.compare(end, 8, "\",\"\"]\n{\"") == 0, "large row ends without truncation");
}
}

int main() {
    try {
        check_wire();
        check_utf8();
        check_large_text();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL trade text companion: %s\n", error.what());
        return 1;
    }
    std::puts("PASS trade text companion");
    return 0;
}
