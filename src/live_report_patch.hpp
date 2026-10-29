#pragma once

#include <pineforge/pineforge.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pineforge::report {

// Owning suffix, explicitly separate from the logical cumulative length.
// No suffix pointer is ever attached to a pf_report_t with a full length.
template<class Row> struct ReportArrayPatch {
    std::size_t offset = 0;
    std::size_t logical_length = 0;
    std::vector<Row> rows;
};

struct LiveReportPatch {
    // Array pointers are null. Length fields describe the cumulative report;
    // storage and indexing belong exclusively to the patches below.
    pf_report_t scalars{};
    ReportArrayPatch<pf_trade_t> trades;
    ReportArrayPatch<pf_equity_point_t> equity;
    ReportArrayPatch<pf_security_diag_t> security;
    ReportArrayPatch<std::uint64_t> hashes;
    ReportArrayPatch<std::string> trace_names;
    std::size_t closed_trades = 0;
    bool interval_ratios = false;
    std::uint64_t ratios_as_of_input = 0;
    std::string chart_timezone;
};

struct LiveReportWork {
    std::uint64_t trade_rows_materialized = 0;
    std::uint64_t trade_rows_folded = 0;
    std::uint64_t equity_rows_materialized = 0;
    std::uint64_t equity_rows_folded = 0;
    std::uint64_t hash_rows_materialized = 0;
    std::uint64_t security_rows_materialized = 0;
    std::uint64_t trace_names_materialized = 0;
    // Warmup is cursor zero, outside the N committed-input updates.
    std::uint64_t warmup_ratio_recomputations = 0;
    std::uint64_t ratio_recomputations = 0;
    std::uint64_t ratio_points_supplied = 0;
    // Explicitly identify reads outside the ordinary live array route. Their
    // existing batch/capture work has no incremental complexity guarantee.
    std::uint64_t fallback_report_reads = 0;

    std::uint64_t non_ratio_rows() const noexcept {
        return trade_rows_materialized + trade_rows_folded
            + equity_rows_materialized + equity_rows_folded
            + hash_rows_materialized + security_rows_materialized + trace_names_materialized;
    }
};

}
