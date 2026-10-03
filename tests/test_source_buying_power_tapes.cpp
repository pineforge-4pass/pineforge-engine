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

namespace {

int passed = 0;
int failed = 0;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();
using Row = std::tuple<std::int64_t, bool, std::int64_t, std::int64_t,
                       std::int64_t, std::int64_t, bool>;

void check(bool condition, const char* expression, int line) {
    if (condition) {
        ++passed;
    } else {
        ++failed;
        std::printf("FAIL source buying power:%d %s\n", line, expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

struct Variant {
    const char* name;
    double capital;
    double tick;
    double step;
    bool on_close;
    int pyramiding;
    bool held_long;
    double held_quantity;
    std::int64_t held_signal;
    std::int64_t entry_signal;
    std::int64_t next_signal;
    std::int64_t close_signal;
    bool bracket;
    bool paired_close;
    std::size_t trades;
};

std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> result;
    std::string field;
    bool quoted = false;
    for (const char character : line) {
        if (character == '"') quoted = !quoted;
        else if (character == ',' && !quoted) {
            result.push_back(field);
            field.clear();
        } else if (character != '\r') field += character;
    }
    result.push_back(field);
    return result;
}

std::int64_t timestamp(const std::string& text) {
    std::tm calendar{};
    std::istringstream stream(text);
    stream >> std::get_time(&calendar, "%Y-%m-%d %H:%M");
    CHECK(!stream.fail());
    return static_cast<std::int64_t>(timegm(&calendar)) * 1000 - 8 * 3600000LL;
}

std::int64_t quantize(double value, double step) {
    return static_cast<std::int64_t>(std::llround(value / step));
}

std::vector<Row> expected(const Variant& variant) {
    const std::string path = std::string(PINEFORGE_BUYING_POWER_FIXTURE_DIR)
        + "/" + variant.name + "/tv_trades.csv";
    std::ifstream stream(path);
    CHECK(stream.good());
    std::string line;
    std::getline(stream, line);
    std::map<int, std::vector<std::string>> entries;
    std::map<int, std::vector<std::string>> exits;
    while (std::getline(stream, line)) {
        const auto record = fields(line);
        if (record.size() < 8) continue;
        if (record[1].find("Entry") == 0) entries[std::stoi(record[0])] = record;
        if (record[1].find("Exit") == 0) exits[std::stoi(record[0])] = record;
    }
    std::vector<Row> result;
    for (const auto& entry : entries) {
        const auto exit = exits.find(entry.first);
        CHECK(exit != exits.end());
        if (exit == exits.end()) continue;
        const auto& opened = entry.second;
        const auto& closed = exit->second;
        result.emplace_back(timestamp(opened[2]), opened[1] == "Entry long",
            quantize(std::stod(opened[4]), variant.tick),
            quantize(std::stod(opened[5]), variant.step), timestamp(closed[2]),
            quantize(std::stod(closed[4]), variant.tick), closed[3] == "Margin call");
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<Bar> feed(const Variant& variant) {
    const std::string path = std::string(PINEFORGE_BUYING_POWER_FIXTURE_DIR)
        + "/" + variant.name + "/bars.csv";
    std::ifstream stream(path);
    CHECK(stream.good());
    std::string line;
    std::getline(stream, line);
    std::vector<Bar> result;
    while (std::getline(stream, line)) {
        const auto record = fields(line);
        if (record.size() < 6) continue;
        Bar bar{};
        bar.timestamp = std::stoll(record[0]);
        bar.open = std::stod(record[1]);
        bar.high = std::stod(record[2]);
        bar.low = std::stod(record[3]);
        bar.close = std::stod(record[4]);
        bar.volume = std::stod(record[5]);
        result.push_back(bar);
    }
    return result;
}

class BuyingPowerHost final : public source::PineStrategyHost {
public:
    explicit BuyingPowerHost(const Variant& variant) : variant_(variant) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config{};
        config.initial_capital = variant.capital;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.process_orders_on_close = variant.on_close;
        config.pyramiding = variant.pyramiding;
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", variant.step);
        syminfo_mintick_ = variant.tick;
    }

    void on_source_bar(const Bar& bar) override {
        if (variant_.held_quantity > 0.0 && bar.timestamp == variant_.held_signal)
            strategy_entry("held", variant_.held_long, missing, missing,
                           variant_.held_quantity);
        if (bar.timestamp == variant_.entry_signal) {
            strategy_entry("reverse", variant_.held_quantity == 0.0 || !variant_.held_long);
            if (variant_.bracket)
                strategy_exit("bracket", "reverse", 12.2, 11.5);
            if (variant_.paired_close) strategy_close("held");
        }
        if (bar.timestamp == variant_.next_signal)
            strategy_entry("reverse-again", true);
        if (bar.timestamp == variant_.close_signal) strategy_close_all();
    }

private:
    const Variant& variant_;
};

void replay(const Variant& variant) {
    std::printf("-- %s\n", variant.name);
    const auto tape = expected(variant);
    const auto bars = feed(variant);
    CHECK(tape.size() == variant.trades);
    CHECK(!bars.empty());
    if (bars.empty()) return;
    BuyingPowerHost host(variant);
    host.set_trade_start_time(bars.front().timestamp);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    std::vector<Row> actual;
    for (int index = 0; index < host.trade_count(); ++index) {
        const auto& trade = host.get_trade(index);
        actual.emplace_back(trade.entry_time, trade.is_long,
            quantize(trade.entry_price, variant.tick), quantize(trade.qty, variant.step),
            trade.exit_time, quantize(trade.exit_price, variant.tick),
            trade.exit_comment == "Margin call");
    }
    std::sort(actual.begin(), actual.end());
    CHECK(actual == tape);
    if (actual != tape)
        std::printf("  actual rows=%zu expected rows=%zu\n", actual.size(), tape.size());
}

}

int main() {
    const Variant variants[] = {
        {"flat-under", 94381.87999999999, .01, 1, false, 0, true, 0, 0,
         1752079500000LL, 0, 1752084000000LL, true, false, 0},
        {"flat-equal", 94381.88, .01, 1, false, 0, true, 0, 0,
         1752079500000LL, 0, 1752084000000LL, true, false, 1},
        {"pooc-under", 100491.23999999989, .01, 1, true, 2, true, 0, 0,
         1775147400000LL, 0, 1775152800000LL, false, false, 0},
        {"pooc-equal", 100491.24, .01, 1, true, 2, true, 0, 0,
         1775147400000LL, 0, 1775152800000LL, false, false, 1},
        {"whole-under", 104012.79999999971, .01, 1, false, 0, true, 9429,
         1754400600000LL, 1754404200000LL, 1754406000000LL, 1754422200000LL,
         false, false, 2},
        {"whole-equal", 104012.8, .01, 1, false, 0, true, 9429,
         1754400600000LL, 1754404200000LL, 1754406000000LL, 1754422200000LL,
         false, false, 5},
        {"fractional-fill-band", 89994.96501299953, .01, .0001, false, 0, true,
         26.5312, 1762419600000LL, 1762421400000LL, 0, 1762426800000LL,
         false, true, 1},
        {"fractional-affordable", 16356.13181699999, .01, .0001, false, 0, true,
         4.2153, 1762027200000LL, 1762034400000LL, 0, 1762048800000LL,
         false, false, 3},
        {"band-reversal-equal", 16355.954142, .01, .0001, false, 0, true,
         4.2153, 1762027200000LL, 1762034400000LL, 0, 1762048800000LL,
         false, true, 3},
        {"band-flat-equal", 16294.24215, .01, .0001, false, 0, true,
         0, 0, 1762034400000LL, 0, 1762048800000LL, false, false, 2},
        {"band-reversal-above", 16355.954184, .01, .0001, false, 0, true,
         4.2153, 1762027200000LL, 1762034400000LL, 0, 1762048800000LL,
         false, true, 3},
        {"band-flat-above", 16294.242192, .01, .0001, false, 0, true,
         0, 0, 1762034400000LL, 0, 1762048800000LL, false, false, 1},
        {"band-reversal-increase", 16355.952678, .01, .0001, false, 0, true,
         4.2152, 1762027200000LL, 1762034400000LL, 0, 1762048800000LL,
         false, true, 3},
        {"band-flat-residue", 89853.02309299953, .01, .0001, false, 0, true,
         0, 0, 1762421400000LL, 0, 1762426800000LL, false, false, 0},
        {"forex-under", 101651.65627359997, .00001, .01, false, 0, false,
         86816.14, 1752560100000LL, 1752563700000LL, 0, 1752573600000LL,
         false, false, 1},
        {"forex-equal", 101651.656274, .00001, .01, false, 0, false,
         86816.14, 1752560100000LL, 1752563700000LL, 0, 1752573600000LL,
         false, false, 2},
    };
    for (const auto& variant : variants) replay(variant);
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
