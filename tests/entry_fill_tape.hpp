#pragma once

#include "exit_comment_tape.hpp"
#include <pineforge/engine.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace entry_fill_tape {

// The ordinary pre-run C API, on a fresh strategy. Neither quantity metadata
// key is supplied; the price tick is a separate symbol fact.
inline void set_eth_symbol(pineforge::BacktestEngine& host, double mintick) {
    strategy_set_syminfo_type(&host, "crypto");
    strategy_set_syminfo_timezone(&host, "UTC");
    strategy_set_syminfo_session(&host, "24x7");
    strategy_set_syminfo_mintick(&host, mintick);
    strategy_set_syminfo_pointvalue(&host, 1.0);
    if (strategy_set_syminfo_string(&host, "tickerid", "BINANCE:ETHUSDT.P") != 0 ||
        strategy_set_syminfo_string(&host, "ticker", "ETHUSDT.P") != 0 ||
        strategy_set_syminfo_string(&host, "currency", "USDT") != 0 ||
        strategy_set_syminfo_string(&host, "basecurrency", "ETH") != 0)
        throw std::runtime_error("could not set the chart symbol");
}

struct Fill {
    std::int64_t time;
    double price;
    double quantity;
};

// Exit slices repeat the entry time and price but list only their own units.
// Some probes label the entry with a comment; a single-entry tape needs no
// signal filter. Fail if the filter would combine different fills.
inline Fill recorded_long(const std::string& path, const std::string& signal = {}) {
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("missing or empty tape: " + path);
    const auto header = exit_comment_tape::csv_fields(line);
    if (header.size() < 6 || header[1] != "Type" || header[2] != "Date and time" ||
        header[3] != "Signal" || header[4] != "Price USDT" || header[5] != "Size (qty)")
        throw std::runtime_error("unexpected trade columns: " + path);
    std::optional<Fill> fill;
    while (std::getline(in, line)) {
        const auto row = exit_comment_tape::csv_fields(line);
        if (row.size() != header.size()) throw std::runtime_error("invalid trade row: " + path);
        if (row[1] != "Entry long" || (!signal.empty() && row[3] != signal)) continue;
        const auto time = exit_comment_tape::tape_time_ms(row[2]);
        const double price = std::stod(row[4]), qty = std::stod(row[5]);
        if (time < 0 || !std::isfinite(price) || !std::isfinite(qty) || qty <= 0)
            throw std::runtime_error("invalid recorded entry: " + path);
        if (!fill) fill = Fill{time, price, 0};
        if (time != fill->time || price != fill->price)
            throw std::runtime_error("ambiguous recorded entry: " + path);
        fill->quantity += qty;
    }
    if (!fill) throw std::runtime_error("no recorded entry: " + path);
    return *fill;
}

inline bool matches(const pineforge::BacktestEngine& host, const std::string& id, Fill tv) {
    if (!host.last_error().empty()) throw std::runtime_error(host.last_error());
    std::optional<double> units;
    for (int i = 0; i < host.report_trade_count(); ++i) {
        const auto row = host.get_report_trade(i);
        if (row.entry_id != id) continue;
        if (row.is_long && row.entry_time == tv.time && std::abs(row.entry_price - tv.price) < 1e-9) {
            units = units.value_or(0.0) + row.qty;
        } else {
            std::fprintf(stderr, "unexpected %s entry %s at %lld ms, price %.17g\n",
                         row.is_long ? "long" : "short", id.c_str(),
                         static_cast<long long>(row.entry_time), row.entry_price);
            return false;
        }
    }
    std::printf("long %s at %lld ms, price %.12g: TradingView qty=%.12g, engine ",
                id.c_str(), static_cast<long long>(tv.time), tv.price, tv.quantity);
    if (!units) {
        std::printf("no entry\n");
        return false;
    }
    std::printf("qty=%.17g\n", *units);
    return std::abs(*units - tv.quantity) < 1e-12;
}

}  // namespace entry_fill_tape
