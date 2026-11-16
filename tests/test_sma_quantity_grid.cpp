// Real-engine caller for the three offsetSteps forms of the SMA quantity
// reconstruction. No arguments: numerical unit controls, not a TV oracle.
// --bars FILE --offset {-0.25,0,0.25}: replay the supplied history, emitting
// every source observation and native report row as binary64 words.
// The host fixture is tests/sma_quantity_host.hpp; the TradingView-bound rows
// are tests/test_sma_quantity_tape.cpp.
#include "sma_quantity_host.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using namespace sma_quantity;

std::vector<Bar> read_bars(const std::string& path) {
    std::ifstream input(path);
    require(input.is_open(), "cannot open bars: " + path);
    std::string line;
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    require(line == "timestamp,open,high,low,close,volume", "invalid bars header");
    std::vector<Bar> bars;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream fields(line);
        fields.imbue(std::locale::classic());
        Bar bar{};
        char comma[5]{};
        fields >> bar.timestamp >> comma[0] >> bar.open >> comma[1]
               >> bar.high >> comma[2] >> bar.low >> comma[3]
               >> bar.close >> comma[4] >> bar.volume;
        require(!fields.fail(), "malformed bar: " + line);
        fields >> std::ws;
        require(fields.eof(), "trailing bar data");
        for (char c : comma) require(c == ',', "invalid bar separator");
        require(std::isfinite(bar.open) && std::isfinite(bar.high)
                    && std::isfinite(bar.low) && std::isfinite(bar.close)
                    && std::isfinite(bar.volume) && bar.low > 0
                    && bar.low <= bar.open && bar.open <= bar.high
                    && bar.low <= bar.close && bar.close <= bar.high && bar.volume >= 0,
                "invalid OHLCV");
        require(bars.empty() || bar.timestamp == bars.back().timestamp + hour,
                "non-hourly or discontinuous history");
        bars.push_back(bar);
    }
    require(input.eof() && !input.bad() && bars.size() == samples
                && bars.front().timestamp == first_time,
            "reconstruction requires its complete 1441-bar history");
    return bars;
}

// Exactly representable price blocks make the quantity boundary independent
// of SMA residual modelling. Neighbouring binary64 source prices exercise
// both sides of a boundary through the same production TA and order path.
std::vector<Bar> control_bars(int prefix, int edge) {
    std::vector<Bar> bars;
    for (int local = -prefix; local < samples + 6; ++local) {
        double price = 125.0 * (1 + (local < 0 ? 0 : (local / 3) % 8));
        if (edge != 0) price = std::nextafter(125.0, edge < 0 ? 0.0 : 250.0);
        // Large off-window sources must not enter the SMA.
        if (local < 0 || local >= samples) price = 1000000.0;
        bars.push_back({price, price, price, price, 1.0, first_time + local * hour});
    }
    return bars;
}

void controls() {
    std::size_t checked = 0;
    for (double offset : {-0.25, 0.0, 0.25}) {
        for (int edge : {-1, 0, 1}) {
            for (int prefix : {0, 9}) {
                const auto bars = control_bars(prefix, edge);
                SmaQuantityHost host(offset);
                host.run(bars.data(), static_cast<int>(bars.size()), "60", "60", false);
                require(host.last_run_status() == 0 && host.last_error().empty(),
                        "control runtime failed: " + host.last_error());
                require(host.observations.size() == bars.size(), "missing source observations");
                require(host.closed_trade_count() == 479 && host.report_trade_count() == 479,
                        "missing, rejected, extra or open control trades");
                std::size_t entries = 0, closes = 0;
                double last = missing;
                for (const auto& row : host.observations) {
                    if (row.local < 2) require(std::isnan(row.average), "NA warmup leaked");
                    if (row.local == samples - 1) last = row.average;
                    if (row.local >= samples) require(row.average == last, "NA tail changed SMA");
                    if (row.close) ++closes;
                    if (!row.entry) continue;
                    const int level = edge == 0 ? 1 + static_cast<int>(entries % 8) : 1;
                    if (edge == 0) require(row.average == 125.0 * level,
                                           "unexpected integer-block SMA");
                    else require(std::isfinite(row.average)
                                     && std::abs(row.average - 125.0) < 1e-10,
                                 "unexpected neighbouring-source SMA");
                    require(row.request >= 0.125 * level - quantity_step
                                && row.request < 0.125 * level + quantity_step,
                            "control request left its boundary cell");
                    const int lots = 12500 * level
                        - (row.request < 0.125 * level ? 1 : 0);
                    // Integer / 100000 selects the nearest double of the exact
                    // decimal lot. Rebuilding lots * binary step can differ.
                    const double expected = static_cast<double>(lots) / 100000.0;
                    const auto trade = host.get_report_trade(static_cast<int>(entries));
                    const std::string label = "offset=" + std::to_string(offset)
                        + " edge=" + std::to_string(edge)
                        + " command=" + std::to_string(row.local);
                    require(trade.qty == expected, label + " fill " + word(trade.qty)
                            + " expected decimal lot " + word(expected));
                    require(trade.is_long && !trade.open_at_end && trade.entry_id == "L",
                            label + " wrong trade identity");
                    require(trade.entry_time == row.time + hour
                                && trade.exit_time == row.time + 2 * hour,
                            label + " changed market schedule");
                    ++entries;
                    ++checked;
                }
                require(entries == 479 && closes == 479 && host.live_position_size() == 0
                            && host.pending_order_count() == 0, "unaccounted control tail");
            }
        }
        // Gridless callers retain the actual production request. A nondecimal
        // grid keeps its binary-grid rule; neither control uses a TV oracle.
        for (double grid : {0.0, 0.03125}) {
            const auto bars = control_bars(9, 0);
            SmaQuantityHost host(offset, grid);
            host.run(bars.data(), static_cast<int>(bars.size()), "60", "60", false);
            require(host.last_run_status() == 0 && host.last_error().empty()
                        && host.report_trade_count() == 479, "unaffected control runtime");
            int entry = 0;
            for (const auto& row : host.observations) {
                if (!row.entry) continue;
                const double expected = grid == 0 ? row.request
                    : static_cast<double>(4 * (1 + entry % 8) - (offset < 0 ? 1 : 0)) / 32.0;
                require(host.get_report_trade(entry).qty == expected,
                        "unaffected quantity changed");
                ++entry;
                ++checked;
            }
        }
    }
    std::cout << "SMA quantity controls: " << checked << " real fills checked\n";
}
}  // namespace

int main(int argc, char** argv) {
    std::cout << std::boolalpha;
    try {
        if (argc == 1) {
            controls();
            return 0;
        }
        require(argc == 5 && std::string(argv[1]) == "--bars"
                    && std::string(argv[3]) == "--offset",
                "usage: test_sma_quantity_grid [--bars FILE --offset {-0.25,0,0.25}]");
        const std::string offset_text = argv[4];
        require(offset_text == "-0.25" || offset_text == "0" || offset_text == "0.25",
                "invalid offset");
        const auto bars = read_bars(argv[2]);
        SmaQuantityHost host(std::stod(offset_text));
        host.run(bars.data(), static_cast<int>(bars.size()), "60", "60", false);
        host.emit();
        return host.last_run_status() == 0 && host.last_error().empty() ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return argc == 1 ? 1 : 2;
    }
}
