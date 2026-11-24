// SPDX-License-Identifier: Apache-2.0
#include "native_startup.hpp"
#include "transport.hpp"

#include <pineforge/native_calendar.hpp>

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
    const auto dump = identity_document(fields, warmup, library);
    const std::string expected =
        std::string("{\"chart_timezone\":\"UTC\",\"input_tf\":\"1\",\"inputs\":[],\"library\":\"") +
        sha256_hex(library) + "\",\"mode\":\"bars\",\"name\":\"strategy\",\"overrides\":[],\"parser\":\"" +
        sha256_hex("") + "\",\"parser_config\":\"" + sha256_hex("{}") +
        "\",\"schema\":\"pineforge-native-ledger/v1\",\"script_tf\":\"3\",\"session\":\"24x7\","
        "\"symbol\":\"TEST:MOCK\",\"syminfo\":{},\"timezone\":\"UTC\",\"warmup\":\"" +
        sha256_hex(warmup) + "\",\"webhook\":\"https://receiver.example/order-actions\"}";
    CHECK(dump == expected);
    CHECK(dump.find("pineforge-native-run/v1") == std::string::npos);
    CHECK(dump.find("timezone_dependency") == std::string::npos);
    CHECK(identity(fields, warmup, library) == sha256_hex(expected));
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
    const auto base = native_identity(n, "bars", "strategy", "http://example/hook", warmup, library);
    auto other = n;
    other.run_number = 2;
    CHECK(native_identity(other, "bars", "strategy", "http://example/hook", warmup, library) != base);
    other = n;
    other.session_key = "live-2";
    CHECK(native_identity(other, "bars", "strategy", "http://example/hook", warmup, library) != base);
    CHECK(native_identity(n, "ticks", "strategy", "http://example/hook", warmup, library) != base);
    CHECK(native_identity(n, "bars", "strategy", "http://example/hook", warmup, "lib-b") != base);
    CHECK(native_identity(n, "bars", "strategy", "http://example/hook", warmup + "x", library) != base);
    other = n;
    other.timezone = "UTC+5";
    validate_native_config(other);
    CHECK(native_identity(other, "bars", "strategy", "http://example/hook", warmup, library) != base);
    other = n;
    other.timezone = "America/New_York";
    validate_native_config(other);
    CHECK(native_identity(other, "bars", "strategy", "http://example/hook", warmup, library) != base);
    other = n;
    other.chart_timezone = "UTC";
    validate_native_config(other);
    CHECK(native_identity(other, "bars", "strategy", "http://example/hook", warmup, library) != base);
    const auto dump = native_identity_document(n, "bars", "strategy", "http://example/hook",
                                               warmup, library);
    CHECK(dump.find("pineforge-native-run/v1") != std::string::npos);
    CHECK(dump.find("unavailable:") == std::string::npos);
    CHECK(dump.find("\"chart_timezone\":\"\"") != std::string::npos);
    CHECK(dump.find("\"chart_timezone_dependency\":null") != std::string::npos);
}

constexpr std::int64_t kDayMs = 86400000;

// 00:00 UTC of a civil date, in Unix milliseconds.
std::int64_t utc(int year, int month, int day) {
    return pineforge::native_calendar::native_civil_days(year, month, day) * kDayMs;
}

// A chart period as the calendar reports it: its nominal open, its first
// trading instant, the next period's nominal open and the next opening the
// calendar expects after it.
pineforge::native_calendar::NativeInterval chart_period(std::int64_t open, std::int64_t first_trade,
                                                        std::int64_t next_open,
                                                        std::int64_t next_trade) {
    pineforge::native_calendar::NativeInterval period;
    period.open_ms = open;
    period.eligible_open_ms = first_trade;
    period.last_traded_close_ms = next_open;
    period.next_period_open_ms = next_open;
    period.next_input_open_ms = next_trade;
    return period;
}

// A period that trades from its own open and is followed at once by the next.
pineforge::native_calendar::NativeInterval chart_period(std::int64_t open, std::int64_t next_open) {
    return chart_period(open, open, next_open, next_open);
}

// The session day of a trading date. It opens `opens_after` milliseconds after
// 00:00 UTC of that date and trades for `trades_for`.
pineforge::native_calendar::NativeSessionDay session_day(int year, int month, int day,
                                                         std::int64_t opens_after = 0,
                                                         std::int64_t trades_for = kDayMs) {
    pineforge::native_calendar::NativeSessionDay out;
    out.ordinal = pineforge::native_calendar::native_civil_days(year, month, day);
    out.origin_ms = out.ordinal * kDayMs + opens_after;
    out.next_origin_ms = out.origin_ms + kDayMs;
    out.spans.emplace_back(out.origin_ms, out.origin_ms + trades_for);
    return out;
}

// True when startup admits the chart; the refusal is printed when it does not.
bool admits(const std::function<void()>& startup) {
    try {
        startup();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "refused: " << e.what() << '\n';
        return false;
    }
}

// The step rule of the startup walk for a chart whose periods restart on
// 1 January (#811): the year's short last period is admitted in its exact
// shape, and every other gap, overlap or short period is refused.
void chart_period_tiling() {
    const auto two = *pineforge::native_calendar::parse_timeframe("2D");
    const auto three = *pineforge::native_calendar::parse_timeframe("3D");
    const auto five = *pineforge::native_calendar::parse_timeframe("5D");
    const auto seven = *pineforge::native_calendar::parse_timeframe("7D");
    const auto two_weeks = *pineforge::native_calendar::parse_timeframe("2W");

    // Whole periods tile: one period read on its second day, and the period
    // right after it.
    CHECK(chart_periods_tile(two, chart_period(utc(2025, 6, 10), utc(2025, 6, 12)),
                             chart_period(utc(2025, 6, 10), utc(2025, 6, 12)),
                             session_day(2025, 6, 11)));
    CHECK(chart_periods_tile(two, chart_period(utc(2025, 6, 10), utc(2025, 6, 12)),
                             chart_period(utc(2025, 6, 12), utc(2025, 6, 14)),
                             session_day(2025, 6, 12)));

    // Admitted: the year's short last period. 2025 has 365 days, so a 2D chart
    // ends it with 31 December alone and a 3D chart with 30 and 31 December;
    // 2028 has 366 days, so a 5D chart ends it with 31 December alone.
    CHECK(chart_periods_tile(two, chart_period(utc(2025, 12, 31), utc(2026, 1, 1)),
                             chart_period(utc(2026, 1, 1), utc(2026, 1, 3)),
                             session_day(2026, 1, 1)));
    CHECK(chart_periods_tile(three, chart_period(utc(2025, 12, 30), utc(2026, 1, 1)),
                             chart_period(utc(2026, 1, 1), utc(2026, 1, 4)),
                             session_day(2026, 1, 1)));
    CHECK(chart_periods_tile(five, chart_period(utc(2028, 12, 31), utc(2029, 1, 1)),
                             chart_period(utc(2029, 1, 1), utc(2029, 1, 6)),
                             session_day(2029, 1, 1)));
    // The same on a session that opens at 09:30: the period opens with the
    // session day of 1 January, not at midnight.
    constexpr std::int64_t opens = 34200000;   // 09:30
    constexpr std::int64_t trades = 23400000;  // until 16:00
    CHECK(chart_periods_tile(two,
                             chart_period(utc(2025, 12, 31) + opens, utc(2026, 1, 1) + opens),
                             chart_period(utc(2026, 1, 1) + opens, utc(2026, 1, 3) + opens),
                             session_day(2026, 1, 1, opens, trades)));

    // Refused: a mid-year gap. On a weekday session the 2D period of Sunday 8
    // and Monday 9 June 2025 first trades on the Monday, three days after the
    // period of Friday 6 June did.
    CHECK(!chart_periods_tile(two,
        chart_period(utc(2025, 6, 6), utc(2025, 6, 6), utc(2025, 6, 8), utc(2025, 6, 9)),
        chart_period(utc(2025, 6, 8), utc(2025, 6, 9), utc(2025, 6, 10), utc(2025, 6, 10)),
        session_day(2025, 6, 9)));
    // Refused: an overlap. The period of 31 December 2024 and 1 January 2025,
    // which the grid counted from 1970 has, under the period that opens on
    // 1 January.
    CHECK(!chart_periods_tile(two,
        chart_period(utc(2024, 12, 31), utc(2024, 12, 31), utc(2025, 1, 2), utc(2025, 1, 1)),
        chart_period(utc(2025, 1, 1), utc(2025, 1, 3)), session_day(2025, 1, 1)));
    // Refused: a short period that does not end at the year boundary, in June
    // and at the start of another month.
    CHECK(!chart_periods_tile(two, chart_period(utc(2025, 6, 10), utc(2025, 6, 11)),
                              chart_period(utc(2025, 6, 11), utc(2025, 6, 13)),
                              session_day(2025, 6, 11)));
    CHECK(!chart_periods_tile(two, chart_period(utc(2025, 1, 31), utc(2025, 2, 1)),
                              chart_period(utc(2025, 2, 1), utc(2025, 2, 3)),
                              session_day(2025, 2, 1)));
    // Refused: a year end in another shape. 1 January 2028 is a Saturday, so
    // on a weekday session the 7D period it opens first trades on Monday
    // 3 January.
    CHECK(!chart_periods_tile(seven,
        chart_period(utc(2027, 12, 31), utc(2027, 12, 31), utc(2028, 1, 1), utc(2028, 1, 3)),
        chart_period(utc(2028, 1, 1), utc(2028, 1, 3), utc(2028, 1, 8), utc(2028, 1, 10)),
        session_day(2028, 1, 3)));
    // A last period that is an hour short of one day, as a clock moved forward
    // on 31 December leaves it.
    CHECK(!chart_periods_tile(two, chart_period(utc(2025, 12, 31), utc(2026, 1, 1) - 3600000),
                              chart_period(utc(2026, 1, 1) - 3600000, utc(2026, 1, 3) - 3600000),
                              session_day(2026, 1, 1, -3600000)));
    // A last period longer than the chart's count, although it ends at 1 January.
    CHECK(!chart_periods_tile(three, chart_period(utc(2025, 12, 28), utc(2026, 1, 1)),
                              chart_period(utc(2026, 1, 1), utc(2026, 1, 4)),
                              session_day(2026, 1, 1)));
    // Weekly periods do not restart with the year, so no short week is admitted.
    CHECK(!chart_periods_tile(two_weeks, chart_period(utc(2025, 12, 29), utc(2026, 1, 1)),
                              chart_period(utc(2026, 1, 1), utc(2026, 1, 15)),
                              session_day(2026, 1, 1)));
}

// The same rule through the startup walk over real calendars.
void multi_day_chart_year_ends() {
    // 2025, 2026 and 2027 have 365 days, so in the three years after
    // 2025-01-01 each of these charts ends every year with a short period:
    // two days for 3D, one for 4D and 7D, five for 6D.
    for (const auto* timeframe : {"3D", "4D", "6D", "7D"})
        CHECK(admits([&] { require_chart_calendar(timeframe, "UTC", utc(2025, 1, 1)); }));
    // 5D has a short period only where a leap year ends: 31 December 2028.
    CHECK(admits([] { require_chart_calendar("5D", "UTC", utc(2028, 1, 1)); }));
    // A session that opens at 09:30 every day: the year's last 2D bar opens at
    // 09:30 on 31 December and the next one at 09:30 on 1 January.
    CHECK(admits([] {
        require_chart_calendar("2D", "UTC", utc(2025, 1, 1) + 34200000, "0930-1600");
    }));
    // Still refused: a short period in the middle of a year. Novosibirsk moved
    // from UTC+6 to UTC+7 on 24 July 2016, a 23-hour day, so that 2D period is
    // 47 hours long.
    CHECK(throws_containing(
        [] { require_chart_calendar("2D", "Asia/Novosibirsk", utc(2016, 1, 1)); },
        "its bars do not tile the calendar's trading days"));
    // Still refused: a year end in another shape. On a weekday session the 7D
    // bar after Friday 31 December 2027 opens on Monday 3 January 2028, because
    // 1 January is a Saturday.
    CHECK(throws_containing(
        [] { require_chart_calendar("7D", "UTC", utc(2027, 1, 1), "24x7:23456"); },
        "its bars do not tile the calendar's trading days"));
}

void chart_calendar_admission() {
    constexpr std::int64_t start = 1735689600000;
    // expectation corrected: confirmed D/W DST refusal -> admission, because
    // civil session successors handle NY/Paris's 71/73 h weekday weekends
    // and 167/169 h weeks (24x7 days: 23/25 h), including NY aliases/POSIX
    // rules; Lord Howe moves 30 minutes (24x7 days: 23.5/24.5 h).
    for (const auto* timeframe : {"D", "1D", "W", "1W"}) {
        for (const auto* timezone : {"UTC", "Etc/UTC", "UTC+05:30", "EST5", "Asia/Taipei", "Asia/Kolkata"})
            require_chart_calendar(timeframe, timezone, start);
        for (const auto* timezone : {"America/New_York", "US/Eastern", "Europe/Paris", "Australia/Lord_Howe",
                                     "EST5EDT", "EST5EDT,M3.2.0,M11.1.0"})
            require_chart_calendar(timeframe, timezone, start); // Calendar successors, not elapsed-day/week tiling.
    }
    for (const auto* timeframe : {"2D", "2W"}) {
        for (const auto* timezone : {"UTC", "Etc/UTC", "UTC+05:30", "EST5", "Asia/Taipei", "Asia/Kolkata"})
            require_chart_calendar(timeframe, timezone, start);
        for (const auto* timezone : {"America/New_York", "US/Eastern", "Europe/Paris", "Australia/Lord_Howe",
                                     "EST5EDT", "EST5EDT,M3.2.0,M11.1.0"})
            CHECK(throws_containing([&] { require_chart_calendar(timeframe, timezone, start); },
                                    "daily/weekly chart delivery on a daylight-saving calendar is not supported yet"));
    }
    for (const auto* timeframe : {"1", "5", "15", "60", "30S"})
        require_chart_calendar(timeframe, "America/New_York", start);
    for (const auto* timeframe : {"240", "120", "45"})
        require_chart_calendar(timeframe, "UTC", start);
    for (const auto* timeframe : {"7", "7S"})
        CHECK(throws_containing([&] { require_chart_calendar(timeframe, "UTC", start); },
                                "its bars do not tile the calendar's trading days"));
    for (const auto* timeframe : {"240", "120", "45"})
        CHECK(throws_containing([&] { require_chart_calendar(timeframe, "America/New_York", start); },
                                "its bars do not tile the calendar's trading days"));
    for (const auto* session : {"24x7", "1700-1700:23456", "0930-1600:23456"}) {
        require_chart_calendar("60", "America/New_York", start, session);
        require_chart_calendar("240", "America/New_York", start, session, false);
        CHECK(throws_containing([&] { require_chart_calendar("240", "America/New_York", start, session); },
                                "its bars do not tile the calendar's trading days"));
    }
    require_chart_calendar("7", "UTC", start, "24x7", false);
    CHECK(throws_containing([&] { require_chart_calendar("D", "America/New_York", start, "24x7", false); },
                            "daily/weekly chart delivery on a daylight-saving calendar is not supported yet"));
    CHECK(throws_containing([&] { require_chart_calendar("60", "Australia/Lord_Howe", start); },
                            "its bars do not tile the calendar's trading days"));
    CHECK(throws_containing([&] { require_chart_calendar("D", "Invalid/Timezone", start); }, "timezone"));
    // expectation corrected: 2040 D refusal -> admission, because future civil
    // session successors also follow DST (23/25 h 24x7 days; 71/73 h weekday weekends).
    require_chart_calendar("D", "America/New_York", 2208988800000);
    CHECK(throws_containing([&] { require_chart_calendar("60", "UTC", start, "0930-1130,1300-1500:23456"); },
                           "its bars do not tile the calendar's trading days"));
    require_chart_calendar("30", "UTC", start, "0930-1130,1300-1500:23456");
    require_chart_calendar("60", "UTC", start, "0900-1200,1300-1500:23456");
    // A reopen 210 minutes after the first opening is on the 15- and 30-minute
    // grids only, so Tokyo and Hong Kong lunch breaks refuse 45, 60 and 120. The
    // Tokyo 120 row is conservative: the bucket that holds the reopen has already
    // traded before the break, keeps its nominal label, and the engine would run
    // it. The check refuses it by name at startup, never mid-run, until
    // calendar-aware chart labels ship.
    // expectation corrected: the reason of the Hong Kong rows, "the reopen is off
    // the chart grid" -> "the check measures the reopen from the first opening",
    // because that session's bars are now counted from 09:00 (TradingView's
    // HKEX:700 charts open 60-minute bars 09:30, 10:00, 11:00, 13:00, 14:00, 15:00
    // and 240-minute bars 09:30, 13:00). Its 13:00 reopen is a bar open at 60 and
    // 120 minutes, so those two rows are conservative too; at 45 it is still
    // inside a bar that opens in the break. The check itself is unchanged.
    for (const auto* timeframe : {"45", "60", "120"}) {
        CHECK(throws_containing([&] { require_chart_calendar(timeframe, "Asia/Tokyo", start, "0900-1130,1230-1500:23456"); },
                               "its bars do not tile the calendar's trading days"));
        CHECK(throws_containing([&] { require_chart_calendar(timeframe, "Asia/Hong_Kong", start, "0930-1200,1300-1600:23456"); },
                               "its bars do not tile the calendar's trading days"));
    }
    for (const auto* timeframe : {"15", "30"}) {
        require_chart_calendar(timeframe, "Asia/Tokyo", start, "0900-1130,1230-1500:23456");
        require_chart_calendar(timeframe, "Asia/Hong_Kong", start, "0930-1200,1300-1600:23456");
    }
    for (const auto* timeframe : {"2D", "3D"}) {
        CHECK(throws_containing([&] { require_chart_calendar(timeframe, "UTC", start, "24x7:23456"); },
                               "its bars do not tile the calendar's trading days"));
    }
    CHECK(throws_containing([&] { require_chart_calendar("D", "Asia/Almaty", 1709078400000); },
                           "its bars do not tile the calendar's trading days"));
    CHECK(throws_containing([&] {
        require_chart_calendar("D", "Asia/Almaty", 1546300800000, "24x7", false, 1735689600000);
    }, "its bars do not tile the calendar's trading days"));
    for (const auto* timeframe : {"2D", "2W"})
        CHECK(throws_containing([&] {
            require_chart_calendar(timeframe, "America/New_York", start, "0930-1600:23456", true);
        }, "only confirmed count-1 daily/weekly bars are supported"));
    CHECK(throws_containing([&] {
        require_chart_calendar("D", "America/New_York", start, "0930-1600:23456", false);
    }, "tick/clock delivery"));
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
    chart_period_tiling();
    multi_day_chart_year_ends();
    chart_calendar_admission();
    if (failures) {
        std::cerr << "test_native_live_startup failures: " << failures << '\n';
        return 1;
    }
    std::cout << "test_native_live_startup: OK\n";
    return 0;
}
