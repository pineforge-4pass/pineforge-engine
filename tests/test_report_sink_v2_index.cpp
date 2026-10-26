#include "sink_v2_index_heap.hpp"

#include <iostream>

namespace {
using namespace sink2_test;

void summary_cap_before_allocation() {
    Directory directory;
    const std::string oversized(sink::ColumnCodec::max_summary_json_bytes + 1, 'x');
    std::size_t diagnostic_peak = 0, diagnostic_allocations = 0;
    for (bool finalize : {false, true}) {
        const auto path = directory.file(finalize ? "oversized-summary" : "diagnostic-control");
        sink::BlockWriter writer(descriptor(path, directory.path), {},
                                 std::make_unique<StoredCodec>());
        writer.write_header({false, false, false, 0, 0});
        writer.write_trailer();
        const auto before = read_file(path);
        bool refused = false;
        {
            const final_heap::Interval interval;
            try {
                if (finalize) writer.finalize(oversized);
                else sink::v2::check_summary_json_length(oversized.size());
            }
            catch (const sink::BlockError& error) {
                refused = error.reason() == sink::BlockFailure::summary_limit;
            }
        }
        require(refused && read_file(path) == before && !writer.receipt().complete,
                "SUMMARY cap refusal appended bytes or used the wrong error");
        if (!finalize) {
            // The exact guard's exception construction is the diagnostic-only
            // control. Neither interval may allocate a SUMMARY/parser buffer.
            diagnostic_peak = final_heap::peak.load();
            diagnostic_allocations = final_heap::allocations.load();
        } else {
            require(final_heap::peak == diagnostic_peak && final_heap::allocations == diagnostic_allocations,
                    "oversized SUMMARY allocated before its length guard");
        }
    }
}

void external_sort() {
    Directory directory;
    const auto path = directory.file("external-sort");
    sink::BlockPolicy policy;
    // 61,793 trades cross the 61,680-tuple initial run by 113 rows.
    // Keep this every-row/rank round trip in the non-slow CTest row.
    const unsigned trades = (1u << 20) / 17 + 113;
    sink::BlockWriter writer(descriptor(path, directory.path), policy, std::make_unique<StoredCodec>());
    writer.write_header({false, false, false, 0, 0});
    std::vector<Row> reference;
    for (unsigned row = 0; row < trades; ++row) {
        pf_trade_t t{};
        t.entry_time = row;
        t.exit_time = static_cast<std::int64_t>((trades - row) % 17);
        t.entry_price = 100.0; t.exit_price = 100.25; t.qty = 1.0;
        t.pnl = row % 31 == 0 ? std::numeric_limits<double>::quiet_NaN()
            : row % 7 == 0 ? -0.0 : static_cast<double>(static_cast<int>(row % 19) - 9);
        t.pnl_pct = row % 3 == 0 ? std::numeric_limits<double>::infinity() : t.pnl / 100.0;
        t.max_runup = static_cast<double>((trades - row) % 23);
        t.max_drawdown = row % 11 == 0 ? -std::numeric_limits<double>::infinity() : -t.max_runup;
        t.commission = 0.125; t.is_long = row % 2;
        reference.push_back({'T', {i(row + 1), s(t.is_long ? "long" : "short"), i(t.entry_time),
            i(t.exit_time), f(t.entry_price), f(t.exit_price), f(t.qty), f(t.pnl), f(t.pnl_pct),
            f(t.max_runup), f(t.max_drawdown), f(t.commission), i(0), i(0), u(0), u(row + 1),
            i(1), s("id"), s(""), s("")}});
        writer.write_t(row + 1, t, row + 1, 1, "id", "", "");
    }
    sealed(writer);
    const auto decoded = decode(read_file(path), equity_types());
    require(decoded.merged == reference, "external sort changed canonical trade rows");
    require(writer.memory().peak_blocks <= writer.memory().block_limit, "index run exceeded builder policy");
}
} // namespace

int main() {
    try {
        summary_cap_before_allocation();
        external_sort(); // retain the original every-row round trip
        std::cout << "sink v2 index: SUMMARY allocation guard and crossing rank round trip passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sink v2 index: " << error.what() << '\n';
        return 1;
    }
}
