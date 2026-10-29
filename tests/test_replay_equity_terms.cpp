#include "report_terminal_quote_fix_fixture.hpp"
#include "../src/report_replay.hpp"

#include <sstream>

using namespace report_modes_fix;

namespace {
class RangeHost final : public source::PineStrategyHost {
public:
    static constexpr double capital = 0x1p53;
    RangeHost() {
        source::PineStrategyConfig config;
        config.initial_capital = capital;
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0 || bar_index_ == 4) strategy_entry("lot", true);
        if (bar_index_ == 2) strategy_close("lot");
    }
};

struct QRow {
    unsigned long long index = 0, settlements = 0;
    long long time = 0;
    double net = 0, term = 0;
    char basis[32]{};
};

QRow last_q(const std::string& path) {
    std::istringstream input(read_sink(path));
    QRow result;
    bool found = false;
    for (std::string line; std::getline(input, line);) {
        if (line.find("[\"Q\",") != 0) continue;
        QRow row;
        require(std::sscanf(line.c_str(), "[\"Q\",%llu,%lld,%llu,%lf,\"%31[^\"]\",%lf]",
                    &row.index, &row.time, &row.settlements, &row.net, row.basis, &row.term) == 6,
                "ordinary Q record lost its operands");
        result = row;
        found = true;
    }
    require(found, "completed replay contains no Q records");
    return result;
}

void nonzero_range_end_lot() {
    RangeHost host;
    const auto path = sink_path("range-terms");
    require(host.set_report_replay_v1(path.c_str(), "equity-terms") == 0, "replay refused");
    std::vector<Bar> bars;
    for (int i = 0; i < 8; ++i) {
        const double price = i == 2 || i == 3 || i >= 6 ? 101.0 : 100.0;
        bars.push_back({price, price, price, price, 1.0, start_ms + i * minute_ms});
    }
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
    require(host.last_error().empty() && host.last_run_status() == 0, "range run failed");
    Report out;
    host.fill_report(&out.value);
    require(out.value.trades_len == 2 && !out.value.trades[0].open_at_end
                && out.value.trades[1].open_at_end, "fixture lacks its close and open lot");
    require(same_bits(out.value.trades[0].pnl, 1.0)
                && same_bits(out.value.trades[1].pnl, 1.0), "input-derived lot PnL changed");
    const auto row = last_q(path);
    require(std::strcmp(row.basis, "range_end") == 0 && row.settlements == 1
                && same_bits(row.net, 1.0) && same_bits(row.term, 1.0),
            "range-end Q lost the physical-close sum or nonzero open lot");
    const double expected = (RangeHost::capital + 1.0) + 1.0;
    require(!same_bits(expected, RangeHost::capital + (1.0 + 1.0)),
            "range-end inputs do not distinguish addition grouping");
    require(out.value.equity_curve_len > 1, "range run has no equity history");
    const auto& last = out.value.equity_curve[out.value.equity_curve_len - 1];
    require(same_bits(last.equity, expected), "range-end equity regrouped the operands");
    require(same_bits(last.open_profit, 0.0), "range-end point retained open profit");
}

void terminal_quote_with_open_lots() {
    QuoteHost host(true);
    const auto path = sink_path("quote-terms");
    require(host.set_report_replay_v1(path.c_str(), "equity-terms") == 0, "replay refused");
    const auto bars = quote_tape();
    run_quote(host, bars);
    Report out;
    host.fill_report(&out.value);
    require_quote_rows(host, out.value);
    const auto row = last_q(path);
    require(std::strcmp(row.basis, "terminal_quote") == 0
                && row.net != 0.0 && row.term != 0.0, "terminal Q lacks nonzero operands");
    require(same_bits(row.term, out.value.net_profit), "terminal term is not presented net");
    const auto& last = out.value.equity_curve[out.value.equity_curve_len - 1];
    require(same_bits(last.equity, 10000.0 + row.term),
            "terminal quote did not use capital plus presented net");
    require(!same_bits(last.equity, (10000.0 + row.net) + row.term),
            "terminal quote inputs do not expose double counting");
}

void basis_arithmetic_witnesses() {
    for (const auto basis : {report::EquityBasis::anchor, report::EquityBasis::terminal_quote,
                             report::EquityBasis::range_end}) {
        const bool anchor = basis == report::EquityBasis::anchor;
        const bool terminal = basis == report::EquityBasis::terminal_quote;
        const auto path = sink_path(anchor ? "anchor-bits" : terminal ? "quote-bits" : "range-bits");
        report::ReportReplay replay(path, "equity-terms");
        replay.begin(scratch_directory(), RangeHost::capital, 1.0, anchor, start_ms, start_ms + minute_ms);
        const double net = terminal || anchor ? 2.0 : 1.0;
        const double term = anchor ? 0.0 : 1.0;
        const double expected = anchor ? RangeHost::capital
            : terminal ? RangeHost::capital + term : (RangeHost::capital + net) + term;
        const pf_equity_point_t point{start_ms, expected, 0.0};
        if (!anchor) {
            replay.point(point, net);
            replay.amend_point(start_ms, basis, term, net);
        }
        auto wrong = point;
        wrong.equity = RangeHost::capital + 2.0;
        require(!same_bits(wrong.equity, expected), "arithmetic negative control is ineffective");
        bool refused = false;
        try { replay.write_q(wrong); }
        catch (const std::logic_error&) { refused = true; }
        require(refused, "replay accepted wrongly grouped presenter equity");
        replay.write_q(point);
        replay.finish(net, {});
        const auto bytes = read_sink(path);
        require_trailer(bytes);
        require(bytes.find(anchor ? "\"anchor\",0.0]" : terminal ? "\"terminal_quote\",1.0]"
                                                               : "\"range_end\",1.0]") != std::string::npos,
                "Q record lost its basis-specific term");
        if (anchor) require(bytes.find(",null,null,\"anchor\",0.0]") != std::string::npos,
                            "anchor exposed a settlement prefix");
    }
}
}

int main() {
    return test_main("test_replay_equity_terms", [] {
        nonzero_range_end_lot();
        terminal_quote_with_open_lots();
        basis_arithmetic_witnesses();
    });
}
