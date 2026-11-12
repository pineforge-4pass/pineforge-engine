#include "fail_alloc_seam.hpp"

#include <pineforge/native_module.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/session_time.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/str_utils.hpp>
#include <pineforge/ta.hpp>

#include "../src/timezone.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#if defined(__linux__)
#include <dlfcn.h>
#endif

namespace {

int failures = 0;
#if defined(__linux__)
thread_local int setenv_failure = 0;
thread_local bool setenv_failed = false;

using SetenvFunction = int (*)(const char*, const char*, int);

SetenvFunction libc_setenv() noexcept {
    static const auto real = reinterpret_cast<SetenvFunction>(::dlsym(RTLD_NEXT, "setenv"));
    return real;
}
#endif
constexpr const char* unknown_zone = "America/Newyork";

void check(bool ok, const char* expression, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        ++failures;
    }
}

#define CHECK(expression) check(bool(expression), #expression, __LINE__)

pf_strategy_t handle(pineforge::BacktestEngine& host) {
    return static_cast<pf_strategy_t>(&host);
}

#if defined(__linux__)
void setenv_failure_rows() {
    const auto real = libc_setenv();
    CHECK(real != nullptr);
    if (real == nullptr) return;
    for (const int error : {ENOMEM, EINVAL}) {
        {
            pineforge::tz_util::ScopedTimezone previous("UTC+9");
        }
        setenv_failed = false;
        setenv_failure = error;
        bool threw = false;
        try {
            pineforge::tz_util::ScopedTimezone target("UTC+3");
        } catch (const std::bad_alloc& exception) {
            threw = true;
            CHECK(error == ENOMEM);
            CHECK(pineforge::classify_run_failure(exception).code
                  == pineforge::RunFailureCode::out_of_memory);
        } catch (const std::system_error& exception) {
            threw = true;
            CHECK(error == EINVAL);
            CHECK(exception.code() == std::error_code(error, std::generic_category()));
            CHECK(pineforge::classify_run_failure(exception).code
                  == pineforge::RunFailureCode::engine_unclassified_error);
            CHECK(std::strstr(exception.what(), "setenv") != nullptr);
        }
        setenv_failure = 0;
        CHECK(setenv_failed);
        CHECK(threw);  // The base ignores errno and keeps the previous zone.
        const std::time_t epoch = 0;
        std::tm local{};
        CHECK(::localtime_r(&epoch, &local) != nullptr);
        CHECK(local.tm_hour == 9);
    }
    pineforge::tz_util::ScopedTimezone target("UTC+3");
    const std::time_t epoch = 0;
    std::tm local{};
    CHECK(::localtime_r(&epoch, &local) != nullptr);
    CHECK(local.tm_hour == 3);
}
#endif

std::string expected_timezone_text(const char* function, const char* zone) {
    return std::string("Error before the first bar: Incorrect `timezone` value for the ") + function
        + "() function: \"" + zone
        + "\". The value must be a string in either UTC/GMT notation "
          "(e.g. \"UTC-5\", \"GMT+0530\") or an IANA time zone database name "
          "(e.g. \"America/New_York\").";
}

std::string expected_timezone_args(const char* function, const char* zone) {
    // These rows use literal names with no JSON escapes.
    return std::string("{\"bar_index\":-1,\"function\":\"") + function
        + "\",\"zone\":\"" + zone + "\"}";
}

void check_timezone_exception(const std::exception& error, const std::string& expected_text,
                              const std::string& expected_args) {
    CHECK(dynamic_cast<const std::runtime_error*>(&error) != nullptr);
    CHECK(dynamic_cast<const pineforge::RunFailureInfo*>(&error) != nullptr);
    const auto value = pineforge::classify_run_failure(error);
    CHECK(std::strcmp(pineforge::run_failure_code_name(value.code), "pine_timezone_error") == 0);
    CHECK(pineforge::run_failure_code_class(value.code) == pineforge::RunFailureClass::strategy);
    CHECK(!pineforge::run_failure_code_retryable(value.code));
    CHECK(value.args && *value.args == expected_args);
    CHECK(std::string(error.what()) == expected_text);
}

void timezone_stop_contract_row() {
#ifdef PINEFORGE_HAS_TIMEZONE_STOP_V1
    static_assert(PINEFORGE_HAS_TIMEZONE_STOP_V1 == 1, "timezone stop feature version");
    using Stop = void (*)(const char*, const char*, const char*);
    static_assert(std::is_same<decltype(&pineforge::pine_timezone_stop), Stop>::value,
                  "timezone stop signature");
    bool threw = false;
    try {
        pineforge::pine_timezone_stop(
            "time", "America/Newyork",
            "Incorrect `timezone` value for the time() function: \"America/Newyork\". "
            "The value must be a string in either UTC/GMT notation "
            "(e.g. \"UTC-5\", \"GMT+0530\") or an IANA time zone database name "
            "(e.g. \"America/New_York\").");
    } catch (const std::exception& error) {
        threw = true;
        check_timezone_exception(error, expected_timezone_text("time", "America/Newyork"),
                                 expected_timezone_args("time", "America/Newyork"));
    }
    CHECK(threw);
#else
    CHECK(false && "PINEFORGE_HAS_TIMEZONE_STOP_V1 is required");
#endif
}

template <class Call>
void prefixed_timezone_row(const char* name, Call call, const char* expected_text,
                           const char* expected_args) {
    bool threw = false;
    try {
        call();
    } catch (const std::exception& error) {
        threw = true;
        check_timezone_exception(error, expected_text, expected_args);
    }
    CHECK(threw);
    std::printf("timezone_bar_context_rows: %s, threw=%d\n", name, int(threw));
}

void timezone_bar_context_rows() {
#ifdef PINEFORGE_HAS_TIMEZONE_STOP_V1
    static_assert(noexcept(pineforge::run_error_bar_index()), "bar index accessor must not throw");
    static_assert(std::is_same<decltype(pineforge::run_error_bar_index()), std::int64_t>::value,
                  "bar index accessor must return an integer");
    CHECK(!pineforge::ta::bar_context().installed);
    CHECK(pineforge::run_error_bar_index() == -1);
    CHECK(pineforge::run_error_bar_prefix() == "Error before the first bar: ");
    prefixed_timezone_row(
        "before_first_bar_hour",
        [] { (void)pineforge::local_hour(0, unknown_zone); },
        "Error before the first bar: Incorrect `timezone` value for the hour() function: "
        "\"America/Newyork\". The value must be a string in either UTC/GMT notation "
        "(e.g. \"UTC-5\", \"GMT+0530\") or an IANA time zone database name "
        "(e.g. \"America/New_York\").",
        R"({"bar_index":-1,"function":"hour","zone":"America/Newyork"})");
    {
        pineforge::ta::BarContextScope scope(7, 0);
        CHECK(pineforge::run_error_bar_index() == 7);
        CHECK(pineforge::run_error_bar_prefix() == "Error on bar 7: ");
        prefixed_timezone_row(
            "bar_7_hour",
            [] { (void)pineforge::local_hour(0, unknown_zone); },
            "Error on bar 7: Incorrect `timezone` value for the hour() function: "
            "\"America/Newyork\". The value must be a string in either UTC/GMT notation "
            "(e.g. \"UTC-5\", \"GMT+0530\") or an IANA time zone database name "
            "(e.g. \"America/New_York\").",
            R"({"bar_index":7,"function":"hour","zone":"America/Newyork"})");
        prefixed_timezone_row(
            "bar_7_time",
            [] { (void)pineforge::timeframe_time(0, "D", "", unknown_zone, "1", "UTC"); },
            "Error on bar 7: Incorrect `timezone` value for the time() function: "
            "\"America/Newyork\". The value must be a string in either UTC/GMT notation "
            "(e.g. \"UTC-5\", \"GMT+0530\") or an IANA time zone database name "
            "(e.g. \"America/New_York\").",
            R"({"bar_index":7,"function":"time","zone":"America/Newyork"})");
        prefixed_timezone_row(
            "bar_7_generated_timestamp",
            [] {
                pineforge::pine_timezone_stop(
                    "timestamp", "Mars/Olympus",
                    "Incorrect `timezone` value for the timestamp() function: \"Mars/Olympus\". "
                    "The value must be a string in either UTC/GMT notation "
                    "(e.g. \"UTC-5\", \"GMT+0530\") or an IANA time zone database name "
                    "(e.g. \"America/New_York\").");
            },
            "Error on bar 7: Incorrect `timezone` value for the timestamp() function: "
            "\"Mars/Olympus\". The value must be a string in either UTC/GMT notation "
            "(e.g. \"UTC-5\", \"GMT+0530\") or an IANA time zone database name "
            "(e.g. \"America/New_York\").",
            R"({"bar_index":7,"function":"timestamp","zone":"Mars/Olympus"})");
    }
    CHECK(!pineforge::ta::bar_context().installed);
    CHECK(pineforge::run_error_bar_index() == -1);
    CHECK(pineforge::run_error_bar_prefix() == "Error before the first bar: ");
#else
    CHECK(false && "PINEFORGE_HAS_TIMEZONE_STOP_V1 is required");
#endif
}

template <class Call>
void invalid_zone_row(const char* function, Call call) {
    bool threw = false;
    try {
        call(std::string(unknown_zone));
    } catch (const std::exception& error) {
        threw = true;
        check_timezone_exception(error, expected_timezone_text(function, unknown_zone),
                                 expected_timezone_args(function, unknown_zone));
    }
    CHECK(threw);  // The base returns a value without rejecting the name.
}

void invalid_component_rows() {
    using Component = int (*)(std::int64_t, const std::string&);
    const struct {
        const char* name;
        Component call;
    } rows[] = {
        {"hour", pineforge::local_hour},
        {"minute", pineforge::local_minute},
        {"second", pineforge::local_second},
        {"dayofmonth", pineforge::local_dayofmonth},
        {"dayofweek", pineforge::local_dayofweek},
        {"month", pineforge::local_month},
        {"year", pineforge::local_year},
        {"weekofyear", pineforge::local_weekofyear},
    };
    for (const auto& row : rows)
        invalid_zone_row(row.name, [&](const std::string& tz) { (void)row.call(0, tz); });
}

void invalid_session_rows() {
    using Call = void (*)(const std::string&);
    const struct {
        const char* name;
        Call call;
    } rows[] = {
        {"time", [](const std::string& tz) {
             (void)pineforge::timeframe_time(0, "D", "", tz, "1", "UTC");
         }},
        {"time", [](const std::string& tz) {
             (void)pineforge::timeframe_time(0, "1", "0000-2400", tz, "1", "UTC");
         }},
        {"time", [](const std::string& tz) {
             (void)pineforge::timeframe_time(0, "D", "", tz, "1", "UTC", "24x7");
         }},
        {"time_close", [](const std::string& tz) {
             (void)pineforge::timeframe_time_close(0, "D", "", tz, "1", "UTC");
         }},
        {"time_close", [](const std::string& tz) {
             (void)pineforge::timeframe_time_close(0, "1", "0000-2400", tz, "1", "UTC");
         }},
        {"time_close", [](const std::string& tz) {
             (void)pineforge::timeframe_time_close(0, "D", "", tz, "1", "UTC", "24x7");
         }},
        {"time", [](const std::string& tz) {
             (void)pineforge::passes_session_filter("24x7", tz, 0);
         }},
        {"time", [](const std::string& tz) {
             (void)pineforge::session_in_market("24x7", tz, 0);
         }},
        {"time", [](const std::string& tz) {
             (void)pineforge::session_in_premarket("24x7", tz, 0);
         }},
        {"time", [](const std::string& tz) {
             (void)pineforge::session_in_postmarket("24x7", tz, 0);
         }},
        {"time", [](const std::string& tz) {
             (void)pineforge::session_in_market("24x7", tz, 0, "1D");
         }},
        {"time", [](const std::string& tz) {
             (void)pineforge::session_in_premarket("24x7", tz, 0, "1D");
         }},
        {"time", [](const std::string& tz) {
             (void)pineforge::session_in_postmarket("24x7", tz, 0, "1D");
         }},
    };
    for (const auto& row : rows) invalid_zone_row(row.name, row.call);
}

void invalid_format_row() {
    invalid_zone_row("format_time", [](const std::string& tz) {
        (void)pineforge::str_format_time(0, "yyyy-MM-dd HH:mm:ss", tz);
    });
}

void accepted_name_controls() {
    // At epoch zero New York is five hours west of UTC (standard time),
    // hence Wednesday 1969-12-31 19:00; UTC is Thursday 1970-01-01.
    // Conventional UTC/GMT offsets have the opposite sign to POSIX TZ.
    // Week numbers retain the Monday-based week-zero convention.
    // Premarket is observed at 07:00 UTC: New York is 02:00, UTC is 07:00,
    // and the positive-offset zones are 10:00 or 10:30. Only the UTC and
    // Etc/UTC rows are in 04:00-09:30.
    constexpr std::int64_t premarket_bar = 7 * 3600000LL;
    const struct {
        const char* name;
        int year, month, day, weekday, week, hour, minute;
        std::int64_t day_open, day_close;
        const char* text;
        bool premarket;
    } rows[] = {
        {"America/New_York", 1969, 12, 31, 4, 52, 19, 0,
         -68400000, 18000000, "1969-12-31 19:00:00", false},
        {"UTC", 1970, 1, 1, 5, 0, 0, 0,
         0, 86400000, "1970-01-01 00:00:00", true},
        {"Etc/UTC", 1970, 1, 1, 5, 0, 0, 0,
         0, 86400000, "1970-01-01 00:00:00", true},
        {"UTC+3", 1970, 1, 1, 5, 0, 3, 0,
         -10800000, 75600000, "1970-01-01 03:00:00", false},
        {"UTC+03:30", 1970, 1, 1, 5, 0, 3, 30,
         -12600000, 73800000, "1970-01-01 03:30:00", false},
        {"GMT+3", 1970, 1, 1, 5, 0, 3, 0,
         -10800000, 75600000, "1970-01-01 03:00:00", false},
    };
    for (const auto& row : rows) {
        const std::string tz(row.name);
        CHECK(pineforge::local_year(0, tz) == row.year);
        CHECK(pineforge::local_month(0, tz) == row.month);
        CHECK(pineforge::local_dayofmonth(0, tz) == row.day);
        CHECK(pineforge::local_dayofweek(0, tz) == row.weekday);
        CHECK(pineforge::local_weekofyear(0, tz) == row.week);
        CHECK(pineforge::local_hour(0, tz) == row.hour);
        CHECK(pineforge::local_minute(0, tz) == row.minute);
        CHECK(pineforge::local_second(0, tz) == 0);
        CHECK(pineforge::str_format_time(0, "yyyy-MM-dd HH:mm:ss", tz) == row.text);
        CHECK(pineforge::timeframe_time(0, "D", "", tz, "1", "UTC") == row.day_open);
        CHECK(pineforge::timeframe_time_close(0, "D", "", tz, "1", "UTC") == row.day_close);
        CHECK(pineforge::session_in_market("1800-2100:1234567", tz, 0)
              == (row.hour >= 18 && row.hour < 21));
        CHECK(pineforge::session_in_premarket("0930-1600:1234567", tz, premarket_bar)
              == row.premarket);
        CHECK(pineforge::session_in_postmarket("0930-1600:1234567", tz, 0)
              == (row.hour >= 16 && row.hour < 20));
    }
}

void memo_controls() {
    const std::string west = "America/New_York";
    const std::string east = "UTC+3";
    for (int i = 0; i < 8; ++i) {
        CHECK(pineforge::local_hour(0, west) == 19);
        CHECK(pineforge::local_hour(0, east) == 3);
    }
    invalid_zone_row("hour", [](const std::string& tz) { (void)pineforge::local_hour(0, tz); });

    (void)pineforge::local_hour(0, west);
    bool correct = true;
    bool threw = false;
    std::size_t attempts = 0;
    bool fired = false;
    {
        fail_alloc::Scope allocation(1);
        try {
            for (int i = 0; i < 8; ++i)
                correct = (pineforge::local_hour(0, west) == 19) && correct;
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        attempts = fail_alloc::attempts();
        fired = allocation.fired();
    }
    CHECK(correct && !threw && !fired && attempts == 0);
}

void alternating_timezone_memo_row() {
    // Construct the names before arming so the row counts the calendar reads,
    // not the caller's temporary string for the sixteen-byte IANA spelling.
    const std::string utc;
    const std::string west = "America/New_York";
    const std::string east = "UTC+3";
    CHECK(pineforge::local_hour(0, utc) == 0);
    CHECK(pineforge::local_hour(0, west) == 19);
    CHECK(pineforge::local_hour(0, east) == 3);
    bool correct = true;
    bool threw = false;
    bool fired = false;
    std::size_t attempts = 0;
    {
        fail_alloc::Scope allocation(1);
        try {
            for (int i = 0; i < 16; ++i) {
                correct = (pineforge::local_hour(0, utc) == 0) && correct;
                correct = (pineforge::local_hour(0, west) == 19) && correct;
                correct = (pineforge::local_hour(0, east) == 3) && correct;
            }
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        attempts = fail_alloc::attempts();
        fired = allocation.fired();
    }
    CHECK(correct && !threw && !fired && attempts == 0);
    std::printf("alternating_timezone_memo_row: %zu allocation attempts\n", attempts);
}

class ZonedHost : public pineforge::source::PineStrategyHost {
public:
    std::array<std::int64_t, 12> readings{};
    int bars_seen = 0;
    std::size_t* first_bar_allocation = nullptr;

    std::int64_t read_chart_day(std::int64_t timestamp_ms) const {
        return fixture_chart_day_key(timestamp_ms);
    }
    std::int64_t read_chart_hour(std::int64_t timestamp_ms) const {
        return fixture_chart_time(timestamp_ms).hour;
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        if (bars_seen == 0 && first_bar_allocation != nullptr)
            *first_bar_allocation = fail_alloc::attempts();
        const auto fields = fixture_chart_time(bar.timestamp);
        const auto day = fixture_chart_day_key(bar.timestamp);
        const std::size_t offset = static_cast<std::size_t>(bars_seen) * 3;
        readings.at(offset) = day;
        readings.at(offset + 1) = fields.dayofmonth;
        readings.at(offset + 2) = fields.hour;
        if (bars_seen == 0) {
            const double missing = std::numeric_limits<double>::quiet_NaN();
            strategy_entry("zone", true, missing, missing,
                           1.0 + static_cast<double>(fields.hour + day) / 1000.0);
        } else if (bars_seen == 1) {
            strategy_close_all();
        }
        ++bars_seen;
    }
};

constexpr std::int64_t first_bar = 1735779600000LL;  // 2025-01-02 01:00 UTC.
const pineforge::Bar zoned_bars[] = {
    {100, 102, 99, 101, 1, first_bar},
    {101, 103, 100, 102, 1, first_bar + 60000},
    {102, 104, 101, 103, 1, first_bar + 120000},
    {103, 105, 102, 104, 1, first_bar + 180000},
};

#if defined(__linux__)
void timezone_system_error_host_rows() {
    ZonedHost host;
    host.set_chart_timezone("America/New_York");
    host.run(zoned_bars, 4);  // Stage the adapter's chart zone through a real begin.
    const pf_strategy_t strategy = handle(host);
    CHECK(strategy_last_run_status(strategy) == 0);
    const char* code = strategy_get_last_error_code(strategy);
    const char* text = strategy_get_last_error(strategy);
    CHECK(code && !*code && text && !*text);

    using Read = std::int64_t (ZonedHost::*)(std::int64_t) const;
    const struct {
        const char* name;
        Read read;
        std::int64_t expected;
    } rows[] = {
        {"chart_day_key", &ZonedHost::read_chart_day, 101},
        {"chart_time", &ZonedHost::read_chart_hour, 20},
    };
    for (const auto& row : rows) {
        {
            pineforge::tz_util::ScopedTimezone previous("UTC+9");
        }
        setenv_failed = false;
        setenv_failure = EINVAL;
        bool escaped = false;
        try {
            (void)(host.*row.read)(first_bar);
        } catch (const std::system_error& error) {
            escaped = true;
            CHECK(error.code() == std::error_code(EINVAL, std::generic_category()));
            CHECK(pineforge::classify_run_failure(error).code
                  == pineforge::RunFailureCode::engine_unclassified_error);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s threw the wrong exception: %s\n", row.name, error.what());
            CHECK(false);
        }
        setenv_failure = 0;
        CHECK(setenv_failed && escaped);
        std::printf("timezone_system_error_host_rows: %s, setenv fired=%d, escaped=%d\n",
                    row.name, int(setenv_failed), int(escaped));
        CHECK((host.*row.read)(first_bar) == row.expected);
    }
}
#endif

template <class T>
void append_bytes(std::string& bytes, const T& value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void append_trade(std::string& bytes, const pineforge::TradeC& trade) {
    // Every report trade field, in order, without indeterminate struct padding.
    append_bytes(bytes, trade.entry_time);
    append_bytes(bytes, trade.exit_time);
    append_bytes(bytes, trade.entry_price);
    append_bytes(bytes, trade.exit_price);
    append_bytes(bytes, trade.pnl);
    append_bytes(bytes, trade.pnl_pct);
    append_bytes(bytes, trade.is_long);
    append_bytes(bytes, trade.max_runup);
    append_bytes(bytes, trade.max_drawdown);
    append_bytes(bytes, trade.qty);
    append_bytes(bytes, trade.commission);
    append_bytes(bytes, trade.entry_bar_index);
    append_bytes(bytes, trade.exit_bar_index);
    append_bytes(bytes, trade.open_at_end);
}

struct Outcome {
    std::size_t allocations = 0;
    std::size_t first_bar_allocation = 0;
    bool fired = false;
    bool escaped = false;
    int status = 0;
    std::string code, text;
    bool report = false;
    int bars = 0;
    int trades = 0;
    std::array<std::int64_t, 12> readings{};
    std::string trade_bytes;
};

Outcome zoned_run(std::size_t nth) {
    ZonedHost host;
    host.set_chart_timezone("America/New_York");
    Outcome result;
    if (nth == 0) host.first_bar_allocation = &result.first_bar_allocation;
    // Setup and report presentation are outside the injection window. Every
    // allocation of the complete four-bar run, including its begin, is swept.
    fail_alloc::arm(nth);
    try {
        host.run(zoned_bars, 4);
    } catch (...) {
        result.escaped = true;
    }
    result.allocations = fail_alloc::attempts();
    result.fired = fail_alloc::disarm();
    const pf_strategy_t strategy = handle(host);
    result.status = strategy_last_run_status(strategy);
    const char* code = strategy_get_last_error_code(strategy);
    const char* text = strategy_get_last_error(strategy);
    result.code = code ? code : "";
    result.text = text ? text : "";
    // The harness refuses a report if any of these three reads signals failure.
    if (result.escaped || result.status == 1 || !result.code.empty() || !result.text.empty())
        return result;

    pineforge::ReportC report{};
    host.fill_report(&report);
    result.report = true;
    result.bars = host.bars_seen;
    result.readings = host.readings;
    result.trades = report.trades_len;
    append_bytes(result.trade_bytes, report.trades_len);
    for (int i = 0; i < report.trades_len; ++i)
        append_trade(result.trade_bytes, report.trades[i]);
    pineforge::BacktestEngine::free_report(&report);
    return result;
}

static std::string compact_allocation_indexes(const std::vector<std::size_t>& indexes) {
    std::string ranges;
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        const std::size_t first = indexes[i];
        while (i + 1 < indexes.size() && indexes[i + 1] == indexes[i] + 1) ++i;
        if (!ranges.empty()) ranges += ',';
        ranges += std::to_string(first);
        if (indexes[i] != first) ranges += "-" + std::to_string(indexes[i]);
    }
    return ranges;
}

constexpr const char* kSetupFailureCodes[] = {
    // AllocationFailure (src/native_run_spec.cpp:872) is refused at
    // src/native_execution_consumer.cpp:2956, with the default at src/native_execution_consumer.hpp:1148.
    "engine_invariant",
    // classify_run_failure maps std::bad_alloc directly at src/run_failure.cpp:425.
    "out_of_memory",
    // tz_read_resource catches append failure at src/native_calendar.cpp:880;
    // missing zone identity is refused at src/native_execution_consumer.cpp:3102.
    "symbol_metadata_rejected",
};

void allocation_differential_row() {
    // Warm run-independent caches before counting the same small run.
    const Outcome warm = zoned_run(0);
    CHECK(warm.report && warm.status == 0);
    const Outcome clean = zoned_run(0);
    CHECK(clean.report && clean.status == 0 && clean.code.empty() && clean.text.empty());
    CHECK(clean.bars == 4 && clean.trades == 1);
    CHECK(clean.readings[0] == 101 && clean.readings[1] == 1 && clean.readings[2] == 20);
    CHECK(clean.allocations > 0);
    if (!clean.report) return;
    std::printf("allocation_differential_row: the first clean bar callback follows run allocation %zu\n",
                clean.first_bar_allocation);
    std::size_t fired = 0;
    std::size_t out_of_memory_failures = 0;
    std::size_t typed_setup_refusals = 0;
    std::map<std::string, std::size_t> setup_codes;
    std::vector<std::size_t> completed_allocations;
    for (std::size_t nth = 1; nth <= clean.allocations; ++nth) {
        const Outcome attempt = zoned_run(nth);
        if (attempt.fired) ++fired;
        bool correct = attempt.fired && !attempt.escaped;
        const bool failed = attempt.status == 1 || !attempt.code.empty() || !attempt.text.empty();
        if (failed) {
            // Pre-begin refusals may retain status 0; the harness still fails
            // them through their code or text and never publishes their rows.
            correct = correct && !attempt.code.empty() && !attempt.text.empty()
                && !attempt.report && attempt.trades == 0 && attempt.trade_bytes.empty();
            // Only the clean run's first callback determines the phase.
            if (nth <= clean.first_bar_allocation) {
                ++setup_codes[attempt.code];
                bool allowed = false;
                for (const char* code : kSetupFailureCodes)
                    if (attempt.code == code) allowed = true;
                correct = correct && allowed;
                if (correct) ++typed_setup_refusals;
            } else {
                correct = correct && attempt.code == "out_of_memory";
                if (correct) ++out_of_memory_failures;
            }
        } else {
            completed_allocations.push_back(nth);
            correct = correct && attempt.report && attempt.status == 0
                && attempt.bars == clean.bars && attempt.readings == clean.readings
                && attempt.trade_bytes == clean.trade_bytes;
        }
        if (!correct) {
            std::fprintf(stderr, "allocation %zu: status=%d code=%s report=%d fired=%d\n",
                         nth, attempt.status, attempt.code.c_str(), int(attempt.report),
                         int(attempt.fired));
            CHECK(correct);
        }
    }
    CHECK(fired == clean.allocations);
    CHECK(out_of_memory_failures > 0);
    std::printf("allocation_differential_row: swept all %zu run allocations, "
                "%zu out_of_memory failures, %zu typed setup refusals\n",
                clean.allocations, out_of_memory_failures, typed_setup_refusals);
    std::string setup_histogram;
    for (const auto& entry : setup_codes) {
        if (!setup_histogram.empty()) setup_histogram += ',';
        setup_histogram += entry.first + "=" + std::to_string(entry.second);
    }
    std::printf("allocation_differential_row: setup-phase codes: %s\n", setup_histogram.c_str());
    const std::string completed_ranges = compact_allocation_indexes(completed_allocations);
    std::printf("allocation_differential_row: %zu of %zu injected runs completed, "
                "at allocation indexes: %s\n",
                completed_allocations.size(), clean.allocations, completed_ranges.c_str());
}

class InvalidZoneHost : public pineforge::NativeStrategyHost {
public:
    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext&) override {
        (void)pineforge::local_hour(bar.timestamp, unknown_zone);
    }
};

void native_module_invalid_name_row() {
    pineforge::native_module::Module<InvalidZoneHost> module;
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "timezone-name";
    spec.identity.run_number = 1;
    spec.input_tf = spec.script_tf = "1";
    spec.ticker = "X";
    spec.tickerid = "EXCHANGE:X";
    spec.type = "crypto";
    spec.currency = spec.basecurrency = "USD";
    spec.description = "native";
    spec.volumetype = "base";
    spec.timezone = spec.chart_timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000;
    spec.point_value = spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = pineforge::NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    CHECK(module.configure_native(spec).status == pineforge::NativeSetupStatus::Applied);
    pf_bar_t bars[] = {{100, 102, 99, 101, 1, first_bar}};
    const pf_strategy_t strategy = handle(module);
    pineforge::native_module::run_backtest<InvalidZoneHost>(strategy, bars, 1, nullptr);
    CHECK(strategy_last_run_status(strategy) == 1);
    const char* code = strategy_get_last_error_code(strategy);
    const char* text = strategy_get_last_error(strategy);
    const char* args = strategy_get_last_error_args(strategy);
    CHECK(code && std::strcmp(code, "pine_timezone_error") == 0);
    // The native callback installs only MapBarContextScope, not a TA context
    // (src/native_execution_consumer.cpp:8856); this fixture inherits no TA bar.
    CHECK(args && std::string(args) == expected_timezone_args("hour", unknown_zone));
    CHECK(text && std::string(text) == expected_timezone_text("hour", unknown_zone));
    // This is the harness's failure predicate: no report is requested on failure.
    const bool failed = strategy_last_run_status(strategy) == 1
        || (code && *code) || (text && *text);
    CHECK(failed);
}

}  // namespace

#if defined(__linux__)
extern "C" int setenv(const char* name, const char* value, int overwrite) noexcept {
    if (setenv_failure != 0 && std::strcmp(name, "TZ") == 0) {
        const int error = setenv_failure;
        setenv_failure = 0;
        setenv_failed = true;
        errno = error;
        return -1;
    }
    if (const auto real = libc_setenv()) return real(name, value, overwrite);
    errno = ENOSYS;
    return -1;
}
#endif

int main() {
#if defined(__linux__)
    setenv_failure_rows();
#endif
    timezone_stop_contract_row();
    timezone_bar_context_rows();
    invalid_component_rows();
    invalid_session_rows();
    invalid_format_row();
    accepted_name_controls();
    memo_controls();
    alternating_timezone_memo_row();
#if defined(__linux__)
    timezone_system_error_host_rows();
#endif
    allocation_differential_row();
    native_module_invalid_name_row();
    return failures == 0 ? 0 : 1;
}
