// The lot-grid regression case (tests/fixtures/qty_step_lot_grid/README.md):
// BINANCE:BTCUSDT 4h, EMA 20/50 crossover, 100 % of equity, 0.1 % commission.
// The strategy is the codegen output of the case's strategy.pine, linked in as
// a fixture object, and runs through the C ABI as the release harness runs it:
// all 2,188 bars, empty input/script timeframe (auto-detected 240), syminfo
// mintick 0.01, pointvalue 1, timezone UTC, session 24x7, and with the grid
// the metadata keys qty_step and mincontract = 1e-05. Every recorded row must
// come back field for field with exact double equality: 22 rows with the
// grid, 27 without.
#include <pineforge/pineforge.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string fixture_file(const char* name) {
    return std::string(PINEFORGE_QTY_STEP_LOT_GRID_FIXTURE_DIR) + "/" + name;
}

std::vector<pf_bar_t> case_bars() {
    std::ifstream input(fixture_file("ohlcv.csv"));
    if (!input) throw std::runtime_error("missing ohlcv.csv");
    std::string line;
    std::getline(input, line);
    std::vector<pf_bar_t> bars;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        pf_bar_t bar{};
        std::stringstream fields(line);
        std::string field;
        std::getline(fields, field, ',');
        bar.timestamp = std::stoll(field);
        double* values[] = {&bar.open, &bar.high, &bar.low, &bar.close, &bar.volume};
        for (double* value : values) {
            std::getline(fields, field, ',');
            *value = std::strtod(field.c_str(), nullptr);
        }
        bars.push_back(bar);
    }
    return bars;
}

// One recorded row: every value as its JSON text.
using Row = std::map<std::string, std::string>;

// The expected-rows files are a JSON array of flat objects (Python json.dump,
// floats in repr, so strtod restores the recorded double exactly).
std::vector<Row> expected_rows(const char* name) {
    std::ifstream input(fixture_file(name));
    if (!input) throw std::runtime_error(std::string("missing ") + name);
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    std::vector<Row> rows;
    std::size_t at = 0;
    while ((at = text.find('{', at)) != std::string::npos) {
        const std::size_t end = text.find('}', at);
        if (end == std::string::npos) throw std::runtime_error("unterminated row");
        Row row;
        std::size_t cursor = at + 1;
        while (true) {
            const std::size_t key_open = text.find('"', cursor);
            if (key_open == std::string::npos || key_open > end) break;
            const std::size_t key_close = text.find('"', key_open + 1);
            const std::string key = text.substr(key_open + 1, key_close - key_open - 1);
            std::size_t value_at = text.find(':', key_close) + 1;
            while (text[value_at] == ' ' || text[value_at] == '\n') ++value_at;
            std::string value;
            if (text[value_at] == '"') {
                const std::size_t value_close = text.find('"', value_at + 1);
                value = text.substr(value_at + 1, value_close - value_at - 1);
                cursor = value_close + 1;
            } else {
                std::size_t value_end = value_at;
                while (value_end < end && text[value_end] != ',' && text[value_end] != '\n')
                    ++value_end;
                value = text.substr(value_at, value_end - value_at);
                cursor = value_end;
            }
            row[key] = value;
        }
        rows.push_back(row);
        at = end + 1;
    }
    return rows;
}

double number(const Row& row, const char* key) {
    const auto found = row.find(key);
    if (found == row.end()) throw std::runtime_error(std::string("row lacks ") + key);
    return std::strtod(found->second.c_str(), nullptr);
}

std::int64_t integer(const Row& row, const char* key) {
    const auto found = row.find(key);
    if (found == row.end()) throw std::runtime_error(std::string("row lacks ") + key);
    return std::stoll(found->second);
}

int run_case(bool with_grid, const char* rows_file, int expected_count) {
    auto bars = case_bars();
    assert(bars.size() == 2188);
    pf_strategy_t strategy = strategy_create(nullptr);
    assert(strategy != nullptr);
    strategy_set_syminfo_mintick(strategy, 0.01);
    strategy_set_syminfo_pointvalue(strategy, 1.0);
    strategy_set_syminfo_timezone(strategy, "UTC");
    strategy_set_syminfo_session(strategy, "24x7");
    if (with_grid) {
        strategy_set_syminfo_metadata(strategy, "qty_step", 1e-05);
        strategy_set_syminfo_metadata(strategy, "mincontract", 1e-05);
    }
    pf_report_t report{};
    run_backtest_full(strategy, bars.data(), static_cast<int>(bars.size()), "", "", 0, 4,
                      static_cast<pf_magnifier_distribution_t>(3), &report);
    const char* error = strategy_get_last_error(strategy);
    int failures = error && *error ? 1 : 0;
    if (failures) std::printf("engine error: %s\n", error);
    const auto expected = expected_rows(rows_file);
    if (static_cast<int>(expected.size()) != expected_count
        || report.trades_len != expected_count) {
        ++failures;
        std::printf("%s: %d engine rows, %zu recorded, %d expected\n", rows_file,
                    report.trades_len, expected.size(), expected_count);
    }
    const int rows = std::min<int>(report.trades_len, static_cast<int>(expected.size()));
    for (int i = 0; i < rows; ++i) {
        const pf_trade_t& got = report.trades[i];
        const Row& want = expected[static_cast<std::size_t>(i)];
        const std::string side = got.is_long ? "long" : "short";
        const char* exit_id = strategy_closed_trade_exit_id(strategy, i);
        const std::string exit_label = got.open_at_end
            ? "OPEN / range-end mark" : std::string(exit_id ? exit_id : "(null)");
        const bool same = integer(want, "n") == i + 1 && want.at("side") == side
            && got.entry_time == integer(want, "entry_time")
            && got.exit_time == integer(want, "exit_time")
            && got.entry_price == number(want, "entry_price")
            && got.exit_price == number(want, "exit_price")
            && got.qty == number(want, "qty") && got.pnl == number(want, "pnl")
            && got.commission == number(want, "commission")
            && strategy_closed_trade_entry_incarnation(strategy, i)
                == static_cast<std::uint64_t>(integer(want, "entry_incarnation"))
            && exit_label == want.at("exit");
        if (!same) {
            ++failures;
            std::printf("%s row %d: engine %s %lld %.17g -> %lld %.17g qty %.17g pnl %.17g"
                        " commission %.17g incarnation %llu exit %s\n",
                        rows_file, i + 1, side.c_str(),
                        static_cast<long long>(got.entry_time), got.entry_price,
                        static_cast<long long>(got.exit_time), got.exit_price, got.qty,
                        got.pnl, got.commission,
                        static_cast<unsigned long long>(
                            strategy_closed_trade_entry_incarnation(strategy, i)),
                        exit_label.c_str());
        }
    }
    std::printf("%s run: %d rows, %d failures\n", with_grid ? "with grid" : "gridless",
                report.trades_len, failures);
    report_free(&report);
    strategy_free(strategy);
    return failures;
}

}  // namespace

int main() {
    int failures = run_case(true, "expected-rows-with-grid.json", 22);
    failures += run_case(false, "expected-rows-without-grid.json", 27);
    std::printf("RESULT: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
