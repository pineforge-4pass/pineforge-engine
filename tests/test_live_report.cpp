// SPDX-License-Identifier: Apache-2.0
#include "../runner/report.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pineforge::live;

namespace {

void merge_fields(Json& destination, const Json& changes) {
    if (changes.kind != Json::Kind::Object) {
        destination = changes;
        return;
    }
    destination.kind = Json::Kind::Object;
    for (const auto& [name, value] : changes.members)
        merge_fields(destination.members[name], value);
}

void apply_delta_for_test(Json& document, const Json& delta) {
    assert(delta.at("schema_version").text() == "pineforge-native-report-delta/v1");
    merge_fields(document, delta.at("fields"));
    for (const auto& [name, patch] : delta.at("arrays").members) {
        auto& owner = name == "closed_trades" ? document : document.members["report"];
        auto& values = owner.members[name];
        const auto offset = patch.at("offset").integer<std::size_t>();
        assert(patch.at("items").kind == Json::Kind::Array);
        assert(offset <= values.items.size());
        values.kind = Json::Kind::Array;
        values.items.resize(offset);
        values.items.insert(values.items.end(), patch.at("items").items.begin(),
                            patch.at("items").items.end());
    }
}

pf_trade_t trade(std::int64_t index, bool open_at_end) {
    pf_trade_t result{};
    result.entry_time = 1000 + index;
    result.exit_time = 2000 + index;
    result.entry_price = 100.0 + index;
    result.exit_price = 101.0 + index;
    result.pnl = 1.0 + index;
    result.pnl_pct = 1.0 + index;
    result.is_long = 1;
    result.max_runup = 2.0 + index;
    result.max_drawdown = 0.5 + index;
    result.qty = 1.0;
    result.commission = 0.1;
    result.entry_bar_index = static_cast<std::int32_t>(index);
    result.exit_bar_index = static_cast<std::int32_t>(index + 1);
    result.open_at_end = open_at_end ? 1 : 0;
    return result;
}

struct ReportInput {
    std::vector<pf_trade_t> trades;
    std::vector<pf_equity_point_t> equity;
    pf_report_t report{};

    ReportInput(std::size_t closed, bool open_at_end, std::size_t points) {
        for (std::size_t index = 0; index < closed; ++index)
            trades.push_back(trade(static_cast<std::int64_t>(index), false));
        if (open_at_end)
            trades.push_back(trade(static_cast<std::int64_t>(closed), true));
        for (std::size_t index = 0; index < points; ++index)
            equity.push_back({static_cast<std::int64_t>(index), 1000.0 + index, 0.25 * index});
        report.total_trades = static_cast<int>(trades.size());
        report.trades = trades.empty() ? nullptr : trades.data();
        report.trades_len = static_cast<int>(trades.size());
        report.net_profit = static_cast<double>(closed);
        report.input_bars_processed = static_cast<std::int64_t>(points);
        report.script_bars_processed = static_cast<std::int64_t>(points);
        report.input_tf_seconds = 60;
        report.script_tf_seconds = 60;
        report.script_tf_ratio = 1;
        report.equity_curve = equity.empty() ? nullptr : equity.data();
        report.equity_curve_len = static_cast<std::int64_t>(equity.size());
    }
};

void equivalence_and_open_boundary() {
    ReportDeltas stream;
    ReportDeltas full(true);
    Json applied;
    for (std::size_t input = 0; input < 16; ++input) {
        ReportInput current(input, input % 3 != 1, input + 1);
        const auto delta = stream.update(current.report, "seeded", input, 9000 + input);
        full.update(current.report, "seeded", input, 9000 + input);
        assert(full.full_json(current.report, "seeded", input, 9000 + input) == full.json());
        apply_delta_for_test(applied, parse_json(delta));
    }
    assert(applied.dump() == full.json());
    assert(stream.closed_trade_visits() < 16 * 5);
    bool refused = false;
    try {
        (void)stream.json();
    } catch (const std::exception&) {
        refused = true;
    }
    assert(refused);
}

struct Growth {
    std::size_t visits = 0;
    std::size_t bytes = 0;
};

Growth growth(std::size_t inputs) {
    ReportDeltas stream;
    Growth result;
    for (std::size_t input = 0; input < inputs; ++input) {
        ReportInput current(input + 1, true, input + 1);
        const auto delta = stream.update(current.report, "growth", input, input);
        result.bytes += delta.size();
    }
    result.visits = stream.closed_trade_visits();
    return result;
}

void growth_is_linear() {
    const auto small = growth(16);
    const auto large = growth(160);
    assert(small.visits <= 3 * 16);
    assert(large.visits <= 3 * 160);
    assert(large.visits < 12 * small.visits);
    assert(large.bytes < 12 * small.bytes);
}

} // namespace

int main() {
    equivalence_and_open_boundary();
    growth_is_linear();
    std::cout << "test_live_report: OK\n";
}
