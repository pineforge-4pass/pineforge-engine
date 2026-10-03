#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace pineforge;

#ifndef PINEFORGE_PERCENT_ENTRY_RESERVATION_FIXTURE_DIR
#error "PINEFORGE_PERCENT_ENTRY_RESERVATION_FIXTURE_DIR must name the tape fixture"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kEntry = 1743775200000LL;
constexpr std::int64_t kAbsent = 1743811200000LL;
constexpr std::int64_t kResume = 1743836400000LL;
constexpr std::int64_t kCleanup = 1743897600000LL;

struct FeedBar {
    std::int64_t timestamp;
    double open;
    double high;
    double low;
    double close;
    double volume;
};

#include "fixtures/percent_entry_reservation/bars.inc"

enum class Shape { ResetFixed, ResetMargin, NewPercent, RearmDefault, NaStanding };
using Row = std::tuple<std::int64_t, bool, long long, long long,
                       std::int64_t, long long>;

std::vector<std::string> cells(const std::string& line) {
    std::vector<std::string> result;
    std::string current;
    bool quoted = false;
    for (const char character : line) {
        if (character == '"') quoted = !quoted;
        else if (character == ',' && !quoted) {
            result.push_back(current);
            current.clear();
        } else if (character != '\r') current += character;
    }
    result.push_back(current);
    return result;
}

std::int64_t tape_time(const std::string& value) {
    std::tm parsed{};
    std::istringstream input(value);
    input >> std::get_time(&parsed, "%Y-%m-%d %H:%M");
    if (input.fail()) return -1;
    return static_cast<std::int64_t>(timegm(&parsed)) * 1000 - 8 * 3600000LL;
}

std::vector<Row> read_tape(const std::string& name) {
    const auto filename = std::string(PINEFORGE_PERCENT_ENTRY_RESERVATION_FIXTURE_DIR)
        + "/" + name + "/tv_trades.csv";
    std::ifstream input(filename);
    if (!input) return {};
    std::string line;
    std::getline(input, line);
    std::map<int, Row> trades;
    while (std::getline(input, line)) {
        const auto fields = cells(line);
        if (fields.size() < 6) continue;
        auto& row = trades[std::stoi(fields[0])];
        const auto price = std::llround(std::stod(fields[4]) * 100.0);
        if (fields[1].find("Entry") == 0) {
            std::get<0>(row) = tape_time(fields[2]);
            std::get<1>(row) = fields[1].find("long") != std::string::npos;
            std::get<2>(row) = price;
            std::get<3>(row) = std::llround(std::stod(fields[5]) * 10000.0);
        } else {
            std::get<4>(row) = tape_time(fields[2]);
            std::get<5>(row) = price;
        }
    }
    std::vector<Row> result;
    for (const auto& trade : trades) result.push_back(trade.second);
    std::sort(result.begin(), result.end());
    return result;
}

class ReservationHost final : public source::PineStrategyHost {
public:
    explicit ReservationHost(Shape shape) : shape_(shape) {
        attach_pine_execution_adapter();
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_metadata("qty_step", 0.0001);
        syminfo_mintick_ = 0.01;
        source::PineStrategyConfig config;
        config.initial_capital = shape == Shape::ResetMargin ? 100805.0 : 1000000.0;
        config.margin_long = shape == Shape::ResetMargin ? 100.0 : 0.0;
        config.margin_short = config.margin_long;
        config.default_qty_type = shape == Shape::ResetMargin
            ? static_cast<int>(QtyType::PERCENT_OF_EQUITY)
            : static_cast<int>(QtyType::FIXED);
        config.default_qty_value = shape == Shape::ResetMargin ? 100.0 : 1.0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar& bar) override {
        const auto timestamp = bar.timestamp;
        if (timestamp == kEntry) {
            if (shape_ == Shape::ResetMargin) strategy_entry("seed", false);
            else strategy_entry("seed", false, kNaN, kNaN,
                shape_ == Shape::NaStanding ? 10.0 : 56.0);
        }
        if (signed_position_size() < 0.0) {
            if (shape_ == Shape::ResetFixed || shape_ == Shape::ResetMargin) {
                const bool absent = timestamp >= kAbsent && timestamp < kResume;
                const double stop = absent ? kNaN : timestamp < kResume ? 1813.82 : 1810.36;
                strategy_exit("take", "seed", absent ? kNaN : 1771.14, kNaN,
                    kNaN, kNaN, kNaN, 50.0);
                strategy_exit("runner", "seed", kNaN, stop, absent ? kNaN : 20.0);
            } else if (shape_ == Shape::NewPercent) {
                if (timestamp < kAbsent)
                    strategy_exit("partial", "seed", kNaN, 1813.82, 20.0,
                        kNaN, kNaN, 50.0);
                if (timestamp >= kResume)
                    strategy_exit("new", "seed", 1771.14, kNaN,
                        kNaN, kNaN, kNaN, 50.0);
            } else if (shape_ == Shape::RearmDefault) {
                if (timestamp < kAbsent)
                    strategy_exit("take", "seed", 1771.14, kNaN,
                        kNaN, kNaN, kNaN, 50.0);
                strategy_exit("runner", "seed", kNaN,
                    timestamp < kResume ? 1813.82 : 1810.36, 20.0);
            } else {
                strategy_exit("target", "seed", timestamp < kAbsent ? 1771.14 : kNaN, kNaN);
            }
        }
        if (timestamp == kCleanup && shape_ != Shape::ResetFixed && shape_ != Shape::ResetMargin)
            strategy_close_all();
    }

private:
    Shape shape_;
};

bool replay(const char* name, Shape shape, std::size_t expected_count) {
    std::vector<Bar> bars;
    for (const auto& source : kBars) {
        Bar bar{};
        bar.timestamp = source.timestamp;
        bar.open = source.open;
        bar.high = source.high;
        bar.low = source.low;
        bar.close = source.close;
        bar.volume = source.volume;
        bars.push_back(bar);
    }
    const auto expected = read_tape(name);
    if (expected.size() != expected_count || bars.empty()) {
        std::printf("FAIL %s fixture rows=%zu expected=%zu\n", name, expected.size(), expected_count);
        return false;
    }
    ReservationHost host(shape);
    host.set_trade_start_time(bars.front().timestamp);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    std::vector<Row> actual;
    for (int index = 0; index < host.trade_count(); ++index) {
        const auto& trade = host.get_trade(index);
        actual.emplace_back(trade.entry_time, trade.is_long,
            std::llround(trade.entry_price * 100.0), std::llround(trade.qty * 10000.0),
            trade.exit_time, std::llround(trade.exit_price * 100.0));
    }
    std::sort(actual.begin(), actual.end());
    const bool passed = host.last_error().empty() && actual == expected;
    std::printf("%s %s trades=%zu tape=%zu error=%s\n", passed ? "PASS" : "FAIL",
        name, actual.size(), expected.size(), host.last_error().c_str());
    if (!passed) {
        for (const auto& row : actual)
            std::printf("engine %lld %d %lld %lld -> %lld %lld\n",
                static_cast<long long>(std::get<0>(row)), std::get<1>(row),
                std::get<2>(row), std::get<3>(row),
                static_cast<long long>(std::get<4>(row)), std::get<5>(row));
    }
    return passed;
}

}

int main() {
    int failures = 0;
    failures += !replay("reset-fixed", Shape::ResetFixed, 2);
    failures += !replay("reset-margin", Shape::ResetMargin, 3);
    failures += !replay("new-percent", Shape::NewPercent, 2);
    failures += !replay("rearm-default", Shape::RearmDefault, 2);
    failures += !replay("nan-standing", Shape::NaStanding, 1);
    return failures == 0 ? 0 : 1;
}
