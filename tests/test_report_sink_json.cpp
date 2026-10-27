// CR-397 red source: first compile this against 30e9f9a0's frozen headers.
// Expected first diagnostic: missing pineforge/report_sink_json.h. The lab
// must record the ACTUAL diagnostic; this comment is not an executed result.
#include <pineforge/report_sink_json.h>
#include "../src/sha256.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct File {
    std::FILE* value = std::tmpfile();
    File() { require(value != nullptr, "tmpfile"); }
    ~File() { std::fclose(value); }
    int fd() const { return ::fileno(value); }
    void put(std::string_view text) {
        require(::ftruncate(fd(), 0) == 0, "truncate");
        require(::pwrite(fd(), text.data(), text.size(), 0)
                    == static_cast<ssize_t>(text.size()), "write fixture");
    }
    std::string read() const {
        struct stat status{};
        require(::fstat(fd(), &status) == 0, "stat fragment");
        std::string text(static_cast<std::size_t>(status.st_size), '\0');
        require(::pread(fd(), text.data(), text.size(), 0)
                    == static_cast<ssize_t>(text.size()), "read fragment");
        return text;
    }
};

// Handwritten v1 schema and handwritten expected report objects. Neither the
// sink writer nor the converter under test manufactures these known answers.
constexpr char header[] =
    "{\"schema\":\"pf-report-sink\",\"version\":1,\"run_id\":\"cr397\","
    "\"engine\":\"test\",\"records\":["
    "{\"type\":\"E\",\"fields\":[\"time_ms\",\"equity\",\"open_profit\"],"
    "\"types\":[\"i64\",\"f64?\",\"f64?\"],"
    "\"units\":[\"unix_ms\",\"account_ccy\",\"account_ccy\"]},"
    "{\"type\":\"T\",\"fields\":[\"n\",\"side\",\"entry_time\",\"exit_time\","
    "\"entry_price\",\"exit_price\",\"qty\",\"pnl\",\"pnl_pct\",\"max_runup\","
    "\"max_drawdown\",\"commission\",\"entry_bar_index\",\"exit_bar_index\","
    "\"open_at_end\",\"entry_incarnation\",\"close_cause\",\"entry_id\","
    "\"exit_id\",\"exit_comment\"],\"types\":[\"i64\",\"str\",\"i64\",\"i64\","
    "\"f64?\",\"f64?\",\"f64?\",\"f64?\",\"f64?\",\"f64?\",\"f64?\",\"f64?\","
    "\"i64\",\"i64\",\"bool\",\"u64\",\"i64\",\"str\",\"str\",\"str\"],"
    "\"units\":[\"\",\"\",\"unix_ms\",\"unix_ms\",\"quote_ccy\",\"quote_ccy\","
    "\"qty\",\"account_ccy\",\"percent\",\"account_ccy\",\"account_ccy\","
    "\"account_ccy\",\"script_bar\",\"script_bar\",\"\",\"\",\"\",\"\",\"\",\"\"]}]}\n";
constexpr char two_rows[] =
    "[\"E\",-0,10000.0,-0.0]\n"
    "[\"\\u0045\",1,10001.25,5e-324]\n";
constexpr char third_row[] = "[\"E\",2,1e+16,1e-05]\n";
constexpr char two_json[] =
    "{\"time_ms\":0,\"equity\":10000.0,\"open_profit\":-0.0},"
    "{\"time_ms\":1,\"equity\":10001.25,\"open_profit\":5e-324}";
constexpr char third_json[] =
    ",{\"time_ms\":2,\"equity\":1e+16,\"open_profit\":1e-05}";
constexpr char trade_one[] =
    "[\"T\",1,\"long\",-0,60000,100.0,110.0,2.0,20.0,10.0,25.0,-2.0,0.5,"
    "0,1,false,9007199254740991,1,\"id\\ud834\\udd1e\",\"exit\",\"quote\\\"\\n\"]\n";
constexpr char trade_two[] =
    "[\"\\u0054\",2,\"short\",60000,120000,110.0,115.0,1.0,-5.0,-5.0,1e+16,-5.0,-0.0,"
    "1,2,true,2,0,\"\",\"\",\"\"]\n";
constexpr char trade_three[] =
    "[\"T\",3,\"long\",120000,180000,115.0,115.0,5e-324,-0.0,1e-05,0.0,-0.0,0.0,"
    "2,3,true,3,0,\"\",\"\",\"\"]\n";
constexpr char trade_one_json[] =
    "{\"n\":1,\"side\":\"long\",\"entry_time\":0,\"exit_time\":60000,"
    "\"entry_price\":100.0,\"exit_price\":110.0,\"qty\":2.0,\"pnl\":20.0,"
    "\"pnl_pct\":10.0,\"max_runup\":25.0,\"max_drawdown\":-2.0,\"commission\":0.5,"
    "\"entry_bar_index\":0,\"exit_bar_index\":1,\"open_at_end\":false,"
    "\"entry_incarnation\":9007199254740991}";
constexpr char trade_two_json[] =
    "{\"n\":2,\"side\":\"short\",\"entry_time\":60000,\"exit_time\":120000,"
    "\"entry_price\":110.0,\"exit_price\":115.0,\"qty\":1.0,\"pnl\":-5.0,"
    "\"pnl_pct\":-5.0,\"max_runup\":1e+16,\"max_drawdown\":-5.0,\"commission\":-0.0,"
    "\"entry_bar_index\":1,\"exit_bar_index\":2,\"open_at_end\":true,\"entry_incarnation\":2}";
constexpr char trade_three_json[] =
    "{\"n\":3,\"side\":\"long\",\"entry_time\":120000,\"exit_time\":180000,"
    "\"entry_price\":115.0,\"exit_price\":115.0,\"qty\":5e-324,\"pnl\":-0.0,"
    "\"pnl_pct\":1e-05,\"max_runup\":0.0,\"max_drawdown\":-0.0,\"commission\":0.0,"
    "\"entry_bar_index\":2,\"exit_bar_index\":3,\"open_at_end\":true,\"entry_incarnation\":3}";

std::string packed(std::string_view rows, int count, int trades = 0) {
    std::string prefix = header;
    prefix.append(rows);
    pineforge::report_sink::Sha256 hash;
    hash.update(prefix);
    return prefix + "{\"trailer\":\"pf-report-sink\",\"version\":1,\"counts\":{\"E\":"
        + std::to_string(count) + ",\"T\":" + std::to_string(trades)
        + "},\"sha256\":\"" + hash.hex_digest() + "\"}\n";
}
struct Call {
    std::array<char, PF_REPORT_SINK_JSON_BUFFER_BYTES> header_bytes{};
    std::array<std::uint8_t, PF_REPORT_SINK_JSON_BUFFER_BYTES> bad_bytes{};
    pf_report_sink_json_request_v1 request{};
    pf_report_sink_json_result_v1 result{};
    Call(int source, int output = -1, int trades = -1) {
        request.struct_size = sizeof request;
        request.version = 1;
        request.operation = PF_SINK_JSON_PREPARE;
        request.sink_fd = source;
        request.trades_fd = trades;
        request.equity_fd = output;
        request.header_buffer = header_bytes.data();
        request.header_capacity = header_bytes.size();
        request.error_buffer = bad_bytes.data();
        request.error_capacity = bad_bytes.size();
        result.struct_size = sizeof result;
        result.version = 1;
    }
    void run() {
        require(pf_report_sink_json_v1(&request, &result) == 0, "valid descriptor refused");
    }
    void guard_from(const Call& prepared, char tag) {
        request.operation = PF_SINK_JSON_GUARD;
        request.equity_fd = -1;
        request.trades_fd = -1;
        request.guard_tag = static_cast<unsigned char>(tag);
        request.expected_ino = prepared.result.source_ino;
        request.expected_size = prepared.result.source_size;
        std::memcpy(request.expected_counts, prepared.result.counts, sizeof request.expected_counts);
        std::memcpy(request.expected_sha256_hex, prepared.result.sha256_hex, 65);
        request.verified_header = prepared.header_bytes.data();
        request.verified_header_bytes = prepared.result.header_bytes;
    }
};
void known_answer(int count) {
    File source, output, trades;
    source.put(packed(std::string(two_rows) + (count == 3 ? third_row : ""), count));
    require(::lseek(source.fd(), 3, SEEK_SET) == 3, "source offset setup");
    require(::lseek(output.fd(), 7, SEEK_SET) == 7, "output offset setup");
    auto call = std::make_unique<Call>(source.fd(), output.fd(), trades.fd());
    call->run();
    require(call->result.status == PF_SINK_JSON_OK && call->result.complete == 1,
            "valid sink not accepted");
    require(call->result.counts[0] == count && call->result.counts[1] == 0, "wrong counts");
    const std::string expected = std::string(two_json) + (count == 3 ? third_json : "");
    require(output.read() == expected, "handwritten E fragment differs");
    require(trades.read().empty() && call->result.trades_json_bytes == 0, "zero-trade fragment");
    require(call->result.equity_json_bytes == expected.size(), "fragment length differs");
    pineforge::report_sink::Sha256 hash;
    hash.update(expected);
    require(hash.hex_digest() == call->result.equity_json_sha256_hex, "fragment digest differs");
    require(::lseek(source.fd(), 0, SEEK_CUR) == 3, "source offset changed");
    require(::lseek(output.fd(), 0, SEEK_CUR) == 7, "output offset changed");
    require(call->result.header_bytes == sizeof(header) - 1, "header length differs");
    require(std::memcmp(call->header_bytes.data(), header, sizeof(header) - 1) == 0,
            "header bytes changed");
    require(call->result.diagnostics.count_e == count, "endpoint count");
    require(call->result.diagnostics.first.time_ms == 0, "integer -0 was not normalized");
    require(call->result.diagnostics.last.time_ms == count - 1, "last endpoint");
    for (char tag : {'T', 'E'}) {
        auto guard = std::make_unique<Call>(source.fd());
        guard->guard_from(*call, tag);
        guard->run();
        require(guard->result.status == PF_SINK_JSON_OK && guard->result.complete, "guard failed");
        require(guard->result.header_bytes == sizeof(header) - 1
                && std::memcmp(guard->header_bytes.data(), header, sizeof(header) - 1) == 0,
                "guard's bounded header differs from the known-valid literal");
    }
    auto guard = std::make_unique<Call>(source.fd());
    guard->guard_from(*call, 'E');
    source.put("malformed\n"); // size mismatch MUST precede header parsing.
    guard->run();
    require(guard->result.status == PF_SINK_JSON_IDENTITY_DIFFERS, "identity lost priority");
}
void failures_and_deferred_result() {
    File source;
    source.put(packed("[\"E\",0,null,0.0]\n", 1));
    auto call = std::make_unique<Call>(source.fd());
    call->run();
    require(call->result.complete == 1 && call->result.status == PF_SINK_JSON_OK,
            "optional wire null must finish strict verification");
    require(call->result.result_tag == 'E' && call->result.result_field == 2,
            "null result witness lost");
    const auto broken = packed("[\"E\",0,1e4,0.0]\n", 1);
    source.put(broken);
    call->run();
    require(call->result.status == PF_SINK_JSON_SOURCE_INVALID && !call->result.complete,
            "correctly hashed noncanonical double accepted");
    require(call->result.error.line_number == 2
                && call->result.error_bytes == sizeof("[\"E\",0,1e4,0.0]\n") - 1,
            "record witness is not the actual bounded offending line");
    require(std::string(reinterpret_cast<char*>(call->bad_bytes.data()), call->result.error_bytes)
                == "[\"E\",0,1e4,0.0]\n", "offending bytes were reconstructed");
    auto bad_descriptor = call->request;
    bad_descriptor.version = 999;
    const auto before = call->result;
    require(pf_report_sink_json_v1(&bad_descriptor, &call->result) == -1,
            "bad descriptor accepted");
    require(std::memcmp(&before, &call->result, sizeof before) == 0, "bad descriptor wrote result");
}
void trade_known_answer(int count) {
    File source, equity, trades;
    // Physical E/T interleaving, with an escaped T tag and audit-only strings.
    std::string rows = "[\"E\",-0,10000.0,-0.0]\n";
    rows += trade_one;
    rows += "[\"\\u0045\",1,10001.25,5e-324]\n";
    rows += trade_two;
    rows += third_row;
    if (count == 3) rows += trade_three;
    source.put(packed(rows, 3, count));
    auto call = std::make_unique<Call>(source.fd(), equity.fd(), trades.fd());
    call->run();
    require(call->result.status == PF_SINK_JSON_OK && call->result.complete, "T prepare incomplete");
    require(call->result.counts[0] == 3 && call->result.counts[1] == count, "E/T counts");
    const std::string expected = std::string(trade_one_json) + "," + trade_two_json
        + (count == 3 ? std::string(",") + trade_three_json : "");
    require(trades.read() == expected, "handwritten T fragment differs");
    require(equity.read() == std::string(two_json) + third_json, "interleaved E fragment differs");
    require(call->result.trades_json_bytes == expected.size(), "T fragment length");
    pineforge::report_sink::Sha256 hash;
    hash.update(expected);
    require(hash.hex_digest() == call->result.trades_json_sha256_hex, "T fragment SHA");
    const auto& summary = call->result.summary;
    require(summary.complete && summary.count_t == count && summary.wins == 1
            && summary.losses == 1 && summary.best_trade == 20.0
            && summary.worst_trade == -5.0 && summary.max_drawdown == -5.0,
            "handwritten T summary differs");
    require(!summary.nonfinite_t_field && !call->result.result_field, "spurious T result failure");
    require(call->result.diagnostics.trade_at_start == 1
            && call->result.diagnostics.first.time_ms == 0
            && call->result.diagnostics.last.time_ms == 2, "T annualization fold");
    for (char tag : {'T', 'E'}) {
        auto guard = std::make_unique<Call>(source.fd());
        guard->guard_from(*call, tag);
        guard->run();
        require(guard->result.status == PF_SINK_JSON_OK && guard->result.complete,
                "T-bearing guard incomplete");
        require(guard->result.counts[tag == 'E' ? 0 : 1] == (tag == 'E' ? 3 : count),
                "T-bearing guard selected count");
    }
}
std::string changed(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    require(at != std::string::npos, "missing mutation target");
    text.replace(at, from.size(), to);
    return text;
}
void trade_refusals_and_priority() {
    File source;
    auto call = std::make_unique<Call>(source.fd());
    const std::array<std::string, 4> malformed{{
        changed(trade_one, "\"long\"", "\"flat\""),
        changed(trade_one, "false", "1"),
        changed(trade_one, "9007199254740991", "-1"),
        changed(trade_one, "100.0", "1e2"),
    }};
    for (const auto& row : malformed) {
        source.put(packed(row, 0, 1));
        call->run();
        require(call->result.status == PF_SINK_JSON_SOURCE_INVALID && !call->result.complete,
                "rehashed malformed T accepted");
        require(call->result.error.line_number == 2, "T source line");
        require(std::string(reinterpret_cast<char*>(call->bad_bytes.data()), call->result.error_bytes)
                == row, "T source witness was not captured");
    }
    const auto null_trade = changed(trade_one, "100.0", "null");
    source.put(packed(std::string("[\"E\",0,null,0.0]\n") + null_trade, 1, 1));
    call->run();
    require(call->result.complete && call->result.result_tag == 'T'
            && call->result.result_field == 5 && call->result.summary.nonfinite_t_field == 5,
            "T finite-result failure must outrank earlier E failure");
    source.put(packed(changed(null_trade, "[\"T\",1,", "[\"T\",2,"), 0, 1));
    call->run();
    require(call->result.complete && call->result.result_tag == 'T'
            && call->result.result_field == 1, "T order must precede T result finiteness");
    source.put(packed(null_trade + std::string("[\"E\",0,1e4,0.0]\n"), 1, 1));
    call->run();
    require(call->result.status == PF_SINK_JSON_SOURCE_INVALID && !call->result.complete
            && call->result.error.line_number == 3, "later source refusal must precede T result");
    const auto first_zero = changed(trade_one, ",20.0,", ",-0.0,");
    const auto second_zero = changed(trade_two, ",-5.0,-5.0,", ",0.0,-5.0,");
    source.put(packed(first_zero + second_zero, 0, 2));
    call->run();
    require(call->result.complete && !call->result.result_field
            && std::signbit(call->result.summary.best_trade)
            && std::signbit(call->result.summary.worst_trade)
            && !std::signbit(call->result.summary.max_drawdown),
            "Python signed-zero ties changed");
    const auto huge_one = changed(trade_one, ",20.0,", ",1.7976931348623157e+308,");
    const auto huge_two = changed(trade_two, ",-5.0,-5.0,", ",1.7976931348623157e+308,-5.0,");
    source.put(packed(huge_one + huge_two, 0, 2));
    call->run();
    require(call->result.complete && call->result.result_tag == 'T'
            && call->result.result_field == 21, "trade drawdown overflow was not deferred");
}
void guard_priority_table() {
    File source;
    const auto original = packed(std::string(two_rows) + trade_one, 2, 1);
    source.put(original);
    auto prepared = std::make_unique<Call>(source.fd());
    prepared->run();
    require(prepared->result.complete && prepared->result.status == PF_SINK_JSON_OK,
            "guard table setup");
    struct Case {
        const char* name;
        std::string bytes;
        std::uint32_t status;
        bool wrong_count;
    };
    for (char tag : {'E', 'T'}) {
        const std::string_view needle = tag == 'E' ? "10000.0" : "100.0";
        const std::string_view bad = tag == 'E' ? "1e+0004" : "1e+02";
        const std::string_view good = tag == 'E' ? "10002.0" : "101.0";
        const std::array<Case, 8> cases{{
            {"identity-before-malformed", "bad\n", PF_SINK_JSON_IDENTITY_DIFFERS, false},
            {"selected-before-digest", changed(original, needle, bad), PF_SINK_JSON_SOURCE_INVALID, false},
            {"digest-before-count", changed(original, needle, good), PF_SINK_JSON_DIGEST_DIFFERS, true},
            {"count-after-equal-digest", original, PF_SINK_JSON_COUNT_DIFFERS, true},
            {"trailer-lf-before-digest", changed(original, needle, good).substr(0, original.size() - 1) + "x",
                PF_SINK_JSON_SOURCE_INVALID, false},
            {"trailer-eof", original.substr(0, original.size() - 2) + "\nx", PF_SINK_JSON_SOURCE_INVALID, false},
            {"raw-trailer", changed(original, "\"trailer\":\"pf-report-sink\"",
                                   "\"trailer\":\"pf-report-sinX\""), PF_SINK_JSON_OK, false},
            {"skip-unrelated-body", tag == 'E' ? changed(original, "\"long\"", "\"flat\"")
                                             : changed(original, "10000.0", "1e+0004"),
                PF_SINK_JSON_DIGEST_DIFFERS, false},
        }};
        for (const auto& item : cases) {
            source.put(item.bytes);
            auto guard = std::make_unique<Call>(source.fd());
            guard->guard_from(*prepared, tag);
            if (item.wrong_count) ++guard->request.expected_counts[tag == 'E' ? 0 : 1];
            guard->run();
            require(guard->result.status == item.status, item.name);
            require(bool(guard->result.complete) == (item.status == PF_SINK_JSON_OK), item.name);
        }
    }
}
} // namespace

int main() {
    try {
        known_answer(2);
        known_answer(3);
        failures_and_deferred_result();
        trade_known_answer(2);
        trade_known_answer(3);
        trade_refusals_and_priority();
        guard_priority_table();
        std::puts("report sink JSON: literal E/T fragments, folds, guards and refusals passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "report sink JSON: %s\n", error.what());
        return 1;
    }
}
