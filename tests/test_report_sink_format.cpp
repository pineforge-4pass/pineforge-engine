// CR-150 lane L0, acceptance (i), unit half (DESIGN.md section 7, 3.7).
//
// RED ON 3fc010a8b: this file does not compile. It includes
// <pineforge/report_sink.h>, ../src/report_sink_writer.hpp and
// ../src/sha256.hpp. None exists before lanes L6a/L6b-1. The first diagnostic
// is the recorded fail-before.
//
// What it asserts once those lanes land:
//   1. F9: the f64 formatter equals CPython 3 repr() over
//        - 1,000,000 finite doubles from splitmix64(0x9E3779B97F4A7C15),
//        - every power of two from 2^-1074 through 2^1023,
//        - 5e-324, DBL_MIN, DBL_MAX, 2^53, 1e15, 1e16, 1e22, 0.0001,
//          0.00001 and -0.0,
//      checked as one SHA-256 of the formatter's text, one value per line.
//      The digest was generated once with CPython 3 repr() on 2026-10-18 and is
//      checked in below (F9; DESIGN.md 3.7, 7).
//      Non-finite values must format as `null`.
//   2. SHA-256 against the NIST FIPS 180-4 vectors.
//   3. Real module: with a sink configured through the C ABI (the tutorial
//      MACD generated.cpp linked into this executable), the file's header,
//      E/T record shape, trailer counts and trailer sha256 match section 3.7;
//      a file cut at any line boundary is refused by the completion rule.
//
// The Python half of (i) - the same trailer checked with hashlib, the
// reference reader refusing every truncation of a small sink, and the -nan
// volume cell round-trip - lives in scripts/check_report_sink_identity.py
// --self-check, which needs the installed run_json path.

#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>

#include "../src/report_sink_writer.hpp"
#include "../src/sha256.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        ++checks;                                                                 \
        if (!(condition)) {                                                       \
            ++failures;                                                           \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);       \
        }                                                                         \
    } while (0)

// ── F9: CPython repr parity ─────────────────────────────────────────────

// Generated once, checked in. The stream below (splitmix64 state seeded with
// 0x9E3779B97F4A7C15, each finite bit pattern taken in turn until 1,000,000
// values, then ldexp(1, e) for e in [-1074, 1023], then the ten specials in
// the order printed below) was formatted with CPython 3 repr(), one value per
// line, and hashed with hashlib.sha256. 2026-10-18.
constexpr const char* kF9Digest =
    "7809a88c86c021f20574465d0a76041e67275235b536dec697c0c669c16a371e";

std::uint64_t f9_state = 0x9E3779B97F4A7C15ull;

std::uint64_t f9_next() {
    f9_state += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = f9_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::vector<double> f9_values() {
    std::vector<double> values;
    values.reserve(1002108);
    while (values.size() < 1000000) {
        const std::uint64_t bits = f9_next();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof value);
        if (std::isfinite(value)) values.push_back(value);
    }
    for (int exponent = -1074; exponent <= 1023; ++exponent)
        values.push_back(std::ldexp(1.0, exponent));
    const double specials[] = {
        5e-324, 2.2250738585072014e-308, 1.7976931348623157e308,
        9007199254740992.0, 1e15, 1e16, 1e22, 0.0001, 0.00001, -0.0};
    for (const double value : specials) values.push_back(value);
    return values;
}

void f64_text_equals_python_repr() {
    const std::vector<double> values = f9_values();
    std::string text;
    text.reserve(values.size() * 24);
    for (const double value : values) {
        text += pineforge::report_sink::f64_text(value);
        text += '\n';
    }
    const std::string digest = pineforge::sha256_hex(text.data(), text.size());
    if (digest != kF9Digest) {
        std::printf("F9 digest: got %s want %s (%zu values)\n", digest.c_str(), kF9Digest,
                    values.size());
        CHECK(digest == kF9Digest);
    }

    // The layout rule spelled out in the design, on the named table.
    CHECK(pineforge::report_sink::f64_text(10000.0) == "10000.0");
    CHECK(pineforge::report_sink::f64_text(0.0001) == "0.0001");
    CHECK(pineforge::report_sink::f64_text(-0.0) == "-0.0");
    CHECK(pineforge::report_sink::f64_text(1e16) == "1e+16");
    CHECK(pineforge::report_sink::f64_text(1.5e-5) == "1.5e-05");
    CHECK(pineforge::report_sink::f64_text(1e22) == "1e+22");
    CHECK(pineforge::report_sink::f64_text(9007199254740992.0) == "9007199254740992.0");

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(pineforge::report_sink::f64_text(nan) == "null");
    CHECK(pineforge::report_sink::f64_text(inf) == "null");
    CHECK(pineforge::report_sink::f64_text(-inf) == "null");
}

// ── SHA-256: NIST FIPS 180-4 vectors ────────────────────────────────────

void sha256_matches_the_nist_vectors() {
    struct Vector {
        std::string message;
        const char* digest;
    };
    const std::vector<Vector> vectors{
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopq"
         "klmnopqrlmnopqrsmnopqrstnopqrstu",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
        {std::string(1000000, 'a'),
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
    };
    for (const Vector& vector : vectors)
        CHECK(pineforge::sha256_hex(vector.message.data(), vector.message.size()) ==
              vector.digest);
}

// ── Real module: sink bytes against section 3.7 ─────────────────────────

extern "C" {
void* strategy_create(const char* params_json);
void run_backtest_full(void* strategy, pf_bar_t* bars, int n, const char* input_tf,
                       const char* script_tf, int bar_magnifier, int magnifier_samples,
                       int magnifier_dist, pf_report_t* out);
void strategy_free(void* strategy);
void report_free(pf_report_t* report);
int strategy_set_report_sink_v1(pf_strategy_t strategy, const pf_report_sink_v1* sink);
int strategy_report_sink_receipt_v1(pf_strategy_t strategy, pf_report_sink_receipt_v1* out);
}

std::string temp_path() {
    char name[] = "/tmp/pf_sink_format_XXXXXX";
    const int fd = mkstemp(name);
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);
    return name;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start + 1));
        start = end + 1;
    }
    return lines;
}

// The completion rule of 3.7, implemented here independently of the writer:
// the last line must be a trailer whose counts and sha256 match every byte
// before it.
bool sink_is_complete(const std::string& text) {
    const std::vector<std::string> lines = lines_of(text);
    if (lines.size() < 2) return false;
    const std::string& trailer = lines.back();
    if (trailer.rfind("{\"trailer\":\"pf-report-sink\",\"version\":1,", 0) != 0)
        return false;
    if (trailer.back() != '\n') return false;

    const std::size_t sha_key = trailer.find("\"sha256\":\"");
    if (sha_key == std::string::npos) return false;
    const std::size_t sha_start = sha_key + std::strlen("\"sha256\":\"");
    const std::size_t sha_end = trailer.find('"', sha_start);
    if (sha_end == std::string::npos || sha_end - sha_start != 64) return false;
    const std::string claimed = trailer.substr(sha_start, 64);

    std::size_t prefix = 0;
    for (std::size_t i = 0; i + 1 < lines.size(); ++i) prefix += lines[i].size();
    const std::string digest = pineforge::sha256_hex(text.data(), prefix);
    if (digest != claimed) return false;

    // Counts: one key per record type in the header.
    const std::string& header = lines.front();
    const std::size_t records = header.find("\"records\":[");
    if (records == std::string::npos) return false;
    const std::size_t counts = trailer.find("\"counts\":{");
    if (counts == std::string::npos) return false;
    for (const char tag : std::string("ETOVKXH")) {
        const std::string marker = std::string("\"") + tag + "\":";
        if (header.find(std::string("\"type\":\"") + tag + "\"") == std::string::npos)
            continue;
        const std::size_t key = trailer.find(marker, counts);
        if (key == std::string::npos) return false;
        std::size_t seen = 0;
        for (std::size_t i = 1; i + 1 < lines.size(); ++i)
            if (lines[i].rfind(std::string("[\"") + tag + "\"", 0) == 0) ++seen;
        const std::size_t value_start = key + marker.size();
        const std::size_t value_end = trailer.find_first_of(",}", value_start);
        if (value_end == std::string::npos) return false;
        if (std::to_string(seen) != trailer.substr(value_start, value_end - value_start))
            return false;
    }
    return true;
}

// Top-level JSON array element count, strings and escapes respected.
std::size_t element_count(const std::string& line) {
    std::size_t count = 1;
    bool in_string = false;
    bool escaped = false;
    for (const char ch : line) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == ',') {
            ++count;
        }
    }
    return line.empty() ? 0 : count;
}

void real_module_writes_the_section_3_7_sink() {
    const std::string path = temp_path();
    constexpr int kBarCount = 4000;
    std::vector<pf_bar_t> bars;
    bars.reserve(kBarCount);
    for (int i = 0; i < kBarCount; ++i) {
        const double price = 100.0 + ((i / 3) % 2 == 0 ? 1.0 : -1.0);
        bars.push_back({price, price + 0.5, price - 0.5, price, 1000.0,
                        1700000000000LL + static_cast<std::int64_t>(i) * 60000LL});
    }

    void* strategy = strategy_create(nullptr);
    CHECK(strategy != nullptr);
    pf_report_sink_v1 sink{};
    sink.struct_size = sizeof(sink);
    sink.version = 1;
    sink.path = path.c_str();
    sink.run_id = "l0-format";
    sink.scratch_dir = nullptr;
    CHECK(strategy_set_report_sink_v1(strategy, &sink) == 0);

    pf_report_t report{};
    run_backtest_full(strategy, bars.data(), kBarCount, "1", "1", 0, 4, 0, &report);
    CHECK(report.total_trades > 0);

    pf_report_sink_receipt_v1 receipt{};
    receipt.struct_size = sizeof(receipt);
    receipt.version = 1;
    CHECK(strategy_report_sink_receipt_v1(strategy, &receipt) == 0);
    CHECK(receipt.complete == 1);
    CHECK(receipt.trades_retained == 0);
    CHECK(receipt.count_t == report.total_trades);
    CHECK(receipt.count_e == report.script_bars_processed);

    const std::string text = read_file(path);
    const std::vector<std::string> lines = lines_of(text);
    CHECK(lines.size() >= 3);
    CHECK(sink_is_complete(text));

    // Header, keys in the order 3.7 states.
    CHECK(lines.front().rfind(
              "{\"schema\":\"pf-report-sink\",\"version\":1,\"run_id\":\"l0-format\","
              "\"engine\":\"",
              0) == 0);
    CHECK(lines.front().find(
              "\"records\":[{\"type\":\"E\",\"fields\":[\"time_ms\",\"equity\","
              "\"open_profit\"],\"types\":[\"i64\",\"f64?\",\"f64?\"],"
              "\"units\":[\"unix_ms\",\"account_ccy\",\"account_ccy\"]},") !=
          std::string::npos);
    CHECK(lines.front().find(
              "{\"type\":\"T\",\"fields\":[\"n\",\"side\",\"entry_time\",\"exit_time\","
              "\"entry_price\",\"exit_price\",\"qty\",\"pnl\",\"pnl_pct\",\"max_runup\","
              "\"max_drawdown\",\"commission\",\"entry_bar_index\",\"exit_bar_index\","
              "\"open_at_end\",\"entry_incarnation\",\"close_cause\",\"entry_id\","
              "\"exit_id\",\"exit_comment\"]") != std::string::npos);
    // Outputs, trace and hash recording are off in this run.
    for (const char tag : std::string("OVKXH"))
        CHECK(lines.front().find(std::string("\"type\":\"") + tag + "\"") ==
              std::string::npos);

    for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
        if (lines[i].rfind("[\"E\"", 0) == 0) CHECK(element_count(lines[i]) == 4);
        if (lines[i].rfind("[\"T\"", 0) == 0) CHECK(element_count(lines[i]) == 21);
    }

    // Truncation is refused at every line boundary but the last.
    std::size_t prefix = 0;
    for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
        prefix += lines[i].size();
        CHECK(!sink_is_complete(text.substr(0, prefix)));
    }

    std::remove(path.c_str());
    report_free(&report);
    strategy_free(strategy);
}

}  // namespace

int main() {
    f64_text_equals_python_repr();
    sha256_matches_the_nist_vectors();
    real_module_writes_the_section_3_7_sink();
    std::printf("report-sink format: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
