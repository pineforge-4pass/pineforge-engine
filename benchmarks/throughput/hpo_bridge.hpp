#pragma once

// Benchmark-module exports, not additions to the kernel's PF_API registry.
// Compile hpo_bridge.cpp into the SAME strategy module as generated.cpp so
// its hidden C++ engine symbols resolve against that module's runtime.
#include <pineforge/pineforge.h>

#include <cstdint>

struct pf_report_summary_v1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t output_mode;
    std::uint32_t complete;
    std::int64_t total_trades;
    std::int64_t equity_point_count;
    std::int64_t input_bars_processed;
    std::int64_t script_bars_processed;
    double net_profit;
    pf_metrics_t metrics;
};

struct pf_hpo_scratch_manifest_v1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint64_t root_bytes;
    std::uint64_t entry_bytes;
    std::uint64_t cohort_bytes;
    std::uint64_t transaction_bytes;
    std::uint64_t resident_bytes;
};

extern "C" {
PF_API std::uint32_t pf_report_output_version_v1();
PF_API int strategy_set_report_output_mode_v1(pf_strategy_t, std::uint32_t mode);
PF_API int strategy_set_hpo_scratch_manifest_v1(
    pf_strategy_t, const pf_hpo_scratch_manifest_v1*);
PF_API int strategy_hpo_scratch_manifest_v1(
    pf_strategy_t, pf_hpo_scratch_manifest_v1*);
PF_API int strategy_run_hpo_v1(pf_strategy_t, const pf_bar_t*, int count);
PF_API int strategy_report_summary_v1(pf_strategy_t, pf_report_summary_v1*);
}
