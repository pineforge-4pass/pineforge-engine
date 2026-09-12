// SPDX-License-Identifier: Apache-2.0
#include "native_startup.hpp"
#include "transport.hpp"

#include <cassert>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pineforge::live;

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " << #cond     \
                      << '\n';                                                      \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

bool throws_containing(const std::function<void()>& fn, const char* needle) {
    try {
        fn();
        return false;
    } catch (const std::exception& e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
}

NativeConfigValues complete_native() {
    NativeConfigValues n;
    n.present = true;
    n.session_key = "live-1";
    n.run_number = 1;
    n.input_tf = "5";
    n.script_tf = "10";
    n.timezone = "UTC";
    n.session = "24x7";
    n.chart_timezone = "";
    n.ticker = "MOCK";
    n.tickerid = "TEST:MOCK";
    n.type = "crypto";
    n.currency = "USDT";
    n.basecurrency = "ETH";
    n.description = "";
    n.volumetype = "";
    n.initial_capital = 10000;
    n.point_value = 1;
    n.account_fx = 1;
    n.price_tick = 0.01;
    n.fee_kind = 2;
    n.fee_value = 6;
    n.allowed_open_directions = 3;
    return n;
}

std::string native_json(const NativeConfigValues& n) {
    auto opt = [](bool present, const std::string& value) {
        return present ? value : "null";
    };
    return std::string("{\"run\":{\"session_key\":\"") + n.session_key +
           "\",\"run_number\":" + std::to_string(n.run_number) +
           "},\"clock\":{\"input_tf\":\"" + n.input_tf + "\",\"script_tf\":\"" + n.script_tf +
           "\",\"timezone\":\"" + n.timezone + "\",\"session\":\"" + n.session +
           "\",\"chart_timezone\":\"" + n.chart_timezone +
           "\"},\"instrument\":{\"ticker\":\"" + n.ticker + "\",\"tickerid\":\"" + n.tickerid +
           "\",\"type\":\"" + n.type + "\",\"currency\":\"" + n.currency +
           "\",\"basecurrency\":\"" + n.basecurrency + "\",\"description\":\"" + n.description +
           "\",\"volumetype\":\"" + n.volumetype +
           "\"},\"execution\":{\"initial_capital\":10000,\"point_value\":1,\"account_fx\":1,"
           "\"price_tick\":0.01,\"slippage_ticks\":0,\"fee_kind\":\"CashPerExecution\","
           "\"fee_value\":6,\"quantity_grid\":" +
           opt(n.optional_mask & 1u, "1") + ",\"close_execution\":\"NextEligiblePoint\","
           "\"max_abs_units\":" +
           opt(n.optional_mask & 2u, "10") + ",\"max_open_lots\":" +
           opt(n.optional_mask & 8u, "4") +
           ",\"allowed_open_directions\":3,\"initial_margin_fraction\":" +
           opt(n.optional_mask & 4u, "0.5") + "}}";
}

pf_bar_t bar(std::int64_t ts) {
    pf_bar_t b{};
    b.timestamp = ts;
    b.open = 100;
    b.high = 102;
    b.low = 99;
    b.close = 101;
    b.volume = 4;
    return b;
}

void legacy_identity_golden() {
    LegacyIdentityFields fields;
    fields.mode = "bars";
    fields.input_tf = "1";
    fields.script_tf = "3";
    fields.session = "24x7";
    fields.timezone = "UTC";
    fields.chart_timezone = "UTC";
    fields.symbol = "TEST:MOCK";
    fields.name = "strategy";
    fields.webhook = "https://receiver.example/order-actions";
    const std::string warmup =
        "timestamp,open,high,low,close,volume\n0,100,102,99,101,4\n";
    const std::string library = "legacy-library-bytes";
    const std::string parser;
    const std::string parser_config = "{}";
    const auto dump =
        identity_document(fields, warmup, library, parser, parser_config);
    const std::string expected =
        std::string("{\"chart_timezone\":\"UTC\",\"input_tf\":\"1\",\"inputs\":[],\"library\":\"") +
        sha256_hex(library) + "\",\"mode\":\"bars\",\"name\":\"strategy\",\"overrides\":[],\"parser\":\"" +
        sha256_hex(parser) + "\",\"parser_config\":\"" + sha256_hex(parser_config) +
        "\",\"schema\":\"pineforge-native-ledger/v1\",\"script_tf\":\"3\",\"session\":\"24x7\","
        "\"symbol\":\"TEST:MOCK\",\"syminfo\":{},\"timezone\":\"UTC\",\"warmup\":\"" +
        sha256_hex(warmup) + "\",\"webhook\":\"https://receiver.example/order-actions\"}";
    CHECK(dump == expected);
    CHECK(dump.find("pineforge-native-run/v1") == std::string::npos);
    CHECK(dump.find("timezone_dependency") == std::string::npos);
    CHECK(identity(fields, warmup, library, parser, parser_config) == sha256_hex(expected));
}

void parse_refusals() {
    CHECK(throws_containing([] { parse_native_config("{"); }, "JSON"));
    CHECK(throws_containing(
        [] {
            parse_native_config(
                "{\"run\":{},\"clock\":{},\"instrument\":{},\"execution\":{},\"x\":1}");
        },
        "unknown field"));
    auto n = complete_native();
    auto text = native_json(n);
    auto parsed = parse_native_config(text);
    CHECK(parsed.session_key == "live-1");
    CHECK(parsed.chart_timezone.empty());
    CHECK(parsed.optional_mask == 0);
    CHECK(throws_containing(
        [] { parse_native_config("{\"run\":{\"session_key\":\"a\",\"run_number\":1},\"clock\":{"
                                 "\"input_tf\":\"1\",\"script_tf\":\"1\",\"timezone\":\"UTC\","
                                 "\"session\":\"24x7\",\"chart_timezone\":\"\"},\"instrument\":{"
                                 "\"ticker\":\"t\",\"tickerid\":\"id\",\"type\":\"\",\"currency\":\"\","
                                 "\"basecurrency\":\"\",\"description\":\"\",\"volumetype\":\"\"},"
                                 "\"execution\":{\"initial_capital\":1,\"point_value\":1,"
                                 "\"account_fx\":1,\"price_tick\":1,\"slippage_ticks\":0,"
                                 "\"fee_kind\":\"Nope\",\"fee_value\":0,\"quantity_grid\":null,"
                                 "\"close_execution\":\"NextEligiblePoint\",\"max_abs_units\":null,"
                                 "\"max_open_lots\":null,\"allowed_open_directions\":3,"
                                 "\"initial_margin_fraction\":null}}"); },
        "fee_kind"));
    n.input_tf = "M";
    n.script_tf = "M";
    CHECK(throws_containing([&] { validate_native_config(n); }, "monthly"));
    n.input_tf = "3M";
    n.script_tf = "3M";
    CHECK(throws_containing([&] { validate_native_config(n); }, "monthly"));
    n = complete_native();
    n.session_key.clear();
    CHECK(throws_containing([&] { validate_native_config(n); }, "empty required"));
    n = complete_native();
    n.timezone = "No/Such_PineForge_Zone";
    CHECK(throws_containing([&] { validate_native_config(n); }, "timezone"));
}

void explicit_cli() {
    auto n = complete_native();
    NativeClockBindings clock;
    clock.input_tf = "1";
    clock.script_tf = "";
    clock.timezone = "UTC";
    clock.session = "24x7";
    clock.chart_timezone = "UTC";
    clock.symbol = "";
    apply_native_config(clock, n);
    CHECK(clock.input_tf == "5");
    CHECK(clock.script_tf == "10");
    CHECK(clock.symbol == "TEST:MOCK");
    CHECK(clock.chart_timezone.empty());
    clock = {};
    clock.input_tf = "1";
    clock.explicit_flags.insert("--input-tf");
    CHECK(throws_containing([&] { apply_native_config(clock, n); }, "contradicts"));
}

void native_history_gaps() {
    auto n = complete_native();
    validate_native_config(n);
    std::vector<pf_bar_t> ok{bar(0), bar(300000), bar(600000), bar(900000)};
    require_native_warmup(n, ok);
    std::vector<pf_bar_t> missing{bar(0), bar(300000), bar(900000)};
    CHECK(throws_containing([&] { require_native_warmup(n, missing); }, "in-session gap"));
    std::vector<pf_bar_t> mid{bar(0), bar(150000)};
    CHECK(throws_containing([&] { require_native_warmup(n, mid); }, "aligned"));
    const auto csv = history("timestamp,open,high,low,close,volume\n"
                             "0,100,102,99,101,4\n300000,100,102,99,101,4\n",
                             true);
    CHECK(csv.size() == 2);
}

void daily_dst_continuity() {
    auto n = complete_native();
    n.input_tf = "D";
    n.script_tf = "D";
    n.timezone = "America/New_York";
    n.session = "24x7";
    validate_native_config(n);
    constexpr std::int64_t mar8 = 1741410000000;
    constexpr std::int64_t mar9 = 1741496400000;
    constexpr std::int64_t mar10 = 1741579200000;
    require_native_warmup(n, std::vector<pf_bar_t>{bar(mar8), bar(mar9), bar(mar10)});
    CHECK(throws_containing(
        [&] {
            require_native_warmup(n, std::vector<pf_bar_t>{bar(mar8), bar(mar10)});
        },
        "in-session gap"));
    n.input_tf = "W";
    n.script_tf = "W";
    n.timezone = "UTC";
    n.session = "24x7";
    validate_native_config(n);
    constexpr std::int64_t week0 = 1736121600000;  // Monday 2025-01-06 00:00 UTC
    constexpr std::int64_t week1 = 1736726400000;  // Monday 2025-01-13 00:00 UTC
    constexpr std::int64_t week3 = 1737331200000;  // Monday 2025-01-20 00:00 UTC
    require_native_warmup(n, std::vector<pf_bar_t>{bar(week0), bar(week1)});
    CHECK(throws_containing(
        [&] {
            require_native_warmup(n, std::vector<pf_bar_t>{bar(week0), bar(week3)});
        },
        "in-session gap"));
}

void timezone_identity() {
    auto utc = timezone_rule_identity("UTC", true);
    CHECK(utc.kind == Json::Kind::Object);
    CHECK(utc.at("kind_name").text() == "utc");
    CHECK(utc.at("effective_definition").text() == "UTC");
    CHECK(utc.dump().find("unavailable:") == std::string::npos);
    auto offset = timezone_rule_identity("UTC+5", true);
    CHECK(offset.at("kind_name").text() == "fixed-offset");
    auto posix = timezone_rule_identity("EST5EDT,M3.2.0,M11.1.0", true);
    CHECK(posix.at("kind_name").text() == "posix-explicit");
    CHECK(posix.at("effective_definition").text() == "EST5EDT,M3.2.0,M11.1.0");
    CHECK(posix.at("resources").items.empty());
    CHECK(posix.dump().find("unavailable:") == std::string::npos);
    auto empty = timezone_rule_identity("", false);
    CHECK(empty.kind == Json::Kind::Null);
    CHECK(throws_containing([] { timezone_rule_identity("", true); }, "timezone"));
    auto ny = timezone_rule_identity("America/New_York", true);
    CHECK(ny.at("kind_name").text() == "tzfile");
    CHECK(ny.at("resources").items.size() == 1);
    CHECK(ny.at("resources").items[0].at("sha256").text().size() == 64);
    CHECK(ny.dump().find("unavailable:") == std::string::npos);
}

void native_identity_changes() {
    auto n = complete_native();
    validate_native_config(n);
    const std::string warmup = "timestamp,open,high,low,close,volume\n0,100,102,99,101,4\n";
    const std::string library = "lib-a";
    const auto base = native_identity(n, "bars", "strategy", "http://example/hook", warmup, library,
                                      "", "{}");
    auto other = n;
    other.run_number = 2;
    CHECK(native_identity(other, "bars", "strategy", "http://example/hook", warmup, library, "",
                          "{}") != base);
    other = n;
    other.session_key = "live-2";
    CHECK(native_identity(other, "bars", "strategy", "http://example/hook", warmup, library, "",
                          "{}") != base);
    CHECK(native_identity(n, "ticks", "strategy", "http://example/hook", warmup, library, "",
                          "{}") != base);
    CHECK(native_identity(n, "bars", "strategy", "http://example/hook", warmup, "lib-b", "",
                          "{}") != base);
    CHECK(native_identity(n, "bars", "strategy", "http://example/hook", warmup + "x", library, "",
                          "{}") != base);
    other = n;
    other.timezone = "UTC+5";
    validate_native_config(other);
    CHECK(native_identity(other, "bars", "strategy", "http://example/hook", warmup, library, "",
                          "{}") != base);
    other = n;
    other.timezone = "America/New_York";
    validate_native_config(other);
    CHECK(native_identity(other, "bars", "strategy", "http://example/hook", warmup, library, "",
                          "{}") != base);
    other = n;
    other.chart_timezone = "UTC";
    validate_native_config(other);
    CHECK(native_identity(other, "bars", "strategy", "http://example/hook", warmup, library, "",
                          "{}") != base);
    const auto dump = native_identity_document(n, "bars", "strategy", "http://example/hook",
                                               warmup, library, "", "{}");
    CHECK(dump.find("pineforge-native-run/v1") != std::string::npos);
    CHECK(dump.find("unavailable:") == std::string::npos);
    CHECK(dump.find("\"chart_timezone\":\"\"") != std::string::npos);
    CHECK(dump.find("\"chart_timezone_dependency\":null") != std::string::npos);
}

}  // namespace

int main() {
    legacy_identity_golden();
    parse_refusals();
    explicit_cli();
    native_history_gaps();
    daily_dst_continuity();
    timezone_identity();
    native_identity_changes();
    if (failures) {
        std::cerr << "test_native_live_startup failures: " << failures << '\n';
        return 1;
    }
    std::cout << "test_native_live_startup: OK\n";
    return 0;
}
