#pragma once

#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace order_print_tape {

constexpr double missing = std::numeric_limits<double>::quiet_NaN();

inline std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> result;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) result.push_back(field);
    return result;
}

inline std::int64_t timestamp(const std::string& stamp) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &year, &month, &day,
                    &hour, &minute) != 5) {
        throw std::runtime_error("invalid TradingView timestamp");
    }
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4
        - year_of_era / 100 + day_of_year;
    const auto days = static_cast<std::int64_t>(era) * 146097 + day_of_era - 719468;
    return ((days * 24 + hour - 8) * 60 + minute) * 60000;
}

inline std::vector<pineforge::Bar> bars(const std::string& fixture) {
    std::ifstream input(std::string(PINEFORGE_ORDER_PRINT_FIXTURE_ROOT)
                        + "/" + fixture + "/bars.csv");
    if (!input) throw std::runtime_error("missing tape bars: " + fixture);
    std::string line;
    std::getline(input, line);
    std::vector<pineforge::Bar> result;
    while (std::getline(input, line)) {
        const auto row = fields(line);
        if (row.size() != 6) throw std::runtime_error("invalid bar row");
        pineforge::Bar bar{};
        bar.timestamp = std::stoll(row[0]);
        bar.open = std::stod(row[1]);
        bar.high = std::stod(row[2]);
        bar.low = std::stod(row[3]);
        bar.close = std::stod(row[4]);
        bar.volume = std::stod(row[5]);
        result.push_back(bar);
    }
    if (result.empty()) throw std::runtime_error("empty tape bars");
    return result;
}

struct TapeTrade {
    std::int64_t entry_time = 0;
    std::int64_t exit_time = 0;
    bool is_long = false;
    double entry_price = missing;
    double exit_price = missing;
    double quantity = missing;
    double profit = missing;
};

inline int compare(pineforge::source::PineStrategyHost& host,
                   const std::string& fixture, const char* timeframe, double tick) {
    const auto feed = bars(fixture);
    host.set_trade_start_time(feed.front().timestamp);
    host.run(feed.data(), static_cast<int>(feed.size()), timeframe, timeframe, false);
    std::ifstream input(std::string(PINEFORGE_ORDER_PRINT_FIXTURE_ROOT)
                        + "/" + fixture + "/tv_trades.csv");
    if (!input) throw std::runtime_error("missing TradingView tape: " + fixture);
    std::string line;
    std::getline(input, line);
    std::map<int, TapeTrade> expected;
    while (std::getline(input, line)) {
        const auto row = fields(line);
        if (row.size() < 8) throw std::runtime_error("invalid TradingView row");
        auto& trade = expected[std::stoi(row[0])];
        if (row[1].find("Entry") == 0) {
            trade.entry_time = timestamp(row[2]);
            trade.entry_price = std::stod(row[4]);
            trade.is_long = row[1] == "Entry long";
        } else {
            trade.exit_time = timestamp(row[2]);
            trade.exit_price = std::stod(row[4]);
        }
        trade.quantity = std::stod(row[5]);
        trade.profit = std::stod(row[7]);
    }
    int failures = !host.last_error().empty()
        || expected.empty() || host.trade_count() != static_cast<int>(expected.size());
    int index = 0;
    for (const auto& item : expected) {
        if (index >= host.trade_count()) break;
        const auto& actual = host.get_trade(index++);
        const auto& wanted = item.second;
        const bool matches = actual.entry_time == wanted.entry_time
            && actual.exit_time == wanted.exit_time && actual.is_long == wanted.is_long
            && std::llround(actual.entry_price / tick) == std::llround(wanted.entry_price / tick)
            && std::llround(actual.exit_price / tick) == std::llround(wanted.exit_price / tick)
            && std::abs(actual.qty - wanted.quantity) < 1e-8
            && std::abs(actual.pnl - wanted.profit) < 1e-4;
        if (!matches) {
            ++failures;
            std::printf("%s: actual %lld @%.8f -> %lld @%.8f qty %.8f pnl %.8f; "
                        "TV %lld @%.8f -> %lld @%.8f qty %.8f pnl %.8f\n",
                        fixture.c_str(), static_cast<long long>(actual.entry_time),
                        actual.entry_price, static_cast<long long>(actual.exit_time),
                        actual.exit_price, actual.qty, actual.pnl,
                        static_cast<long long>(wanted.entry_time), wanted.entry_price,
                        static_cast<long long>(wanted.exit_time), wanted.exit_price,
                        wanted.quantity, wanted.profit);
        }
    }
    std::printf("%s: %zu tape trades, %d engine trades, %d failures %s\n",
                fixture.c_str(), expected.size(), host.trade_count(), failures,
                host.last_error().c_str());
    return failures;
}

}
