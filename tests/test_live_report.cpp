// SPDX-License-Identifier: Apache-2.0
#include "../runner/report.hpp"
#include "live_report_document_oracle.hpp"

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
        assert(native_report_document(current.report, "seeded", input, 9000 + input).dump() == full.full_json());
        assert(full.full_json() == full.json());
        apply_delta_for_test(applied, parse_json(delta));
        assert(applied.dump() == full.json());
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

void revised_last_closed_trade() {
    for (const bool retain_document : {false, true}) {
        ReportDeltas stream(retain_document);
        ReportInput current(3, true, 4);
        Json applied;
        apply_delta_for_test(applied, parse_json(stream.update(current.report, "revision", 1, 100)));
        assert(applied.dump() == native_report_document(current.report, "revision", 1, 100).dump());
        current.trades[2].max_runup += 17.25;
        const auto delta = parse_json(stream.update(current.report, "revision", 2, 101));
        assert(delta.at("arrays").at("closed_trades").at("offset").integer<std::size_t>() == 2);
        apply_delta_for_test(applied, delta);
        assert(applied.dump() == native_report_document(current.report, "revision", 2, 101).dump());
        assert(applied.at("closed_trades").items[2].at("max_runup").real() == 21.25);
        if (retain_document) assert(stream.json() == applied.dump());
    }
}

void closed_prefix_regression_is_refused() {
    for (const bool retain_document : {false, true}) {
        for (const bool shrink : {false, true}) {
            ReportDeltas stream(retain_document);
            ReportInput previous(3, true, 4);
            stream.update(previous.report, "regression", 1, 100);
            ReportInput current(shrink ? 2 : 3, false, 4);
            if (!shrink) current.trades[2].open_at_end = 1;
            bool refused = false;
            try {
                stream.update(current.report, "regression", 2, 101);
            } catch (const std::runtime_error& error) {
                refused = std::string(error.what()) == "cumulative report closed-trade prefix regressed";
            }
            assert(refused);
        }
    }
}

struct Growth {
    std::size_t visits = 0;
    std::size_t bytes = 0;
};

Growth growth(std::size_t inputs, bool retain_document) {
    ReportDeltas stream(retain_document);
    Growth result;
    for (std::size_t input = 0; input < inputs; ++input) {
        ReportInput current(input + 1, true, input + 1);
        const auto delta = stream.update(current.report, "growth", input, input);
        result.bytes += retain_document ? stream.json().size() : delta.size();
    }
    result.visits = stream.closed_trade_visits();
    return result;
}

void growth_is_linear() {
    // These are the shared extraction paths for off/delta and plain/full.
    for (const bool retain_document : {false, true}) {
        const auto small = growth(16, retain_document);
        const auto large = growth(160, retain_document);
        assert(small.visits <= 3 * 16);
        assert(large.visits <= 3 * 160);
        assert(large.visits < 12 * small.visits);
        if (retain_document) assert(large.bytes > 12 * small.bytes);
        else assert(large.bytes < 12 * small.bytes);
    }
}

} // namespace

int main() {
    equivalence_and_open_boundary();
    revised_last_closed_trade();
    closed_prefix_regression_is_refused();
    growth_is_linear();
    std::cout << "test_live_report: OK\n";
}
