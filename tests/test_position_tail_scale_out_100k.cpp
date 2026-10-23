// Keep the R11 N^2 control, linear-work assertions and complete L18 100k
// T/H/E/metrics differential, with real work reported from its adapter TU.
#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace r11_tail_work {
using Adapter = pineforge::source::PineExecutionAdapter;
using Work = Adapter::PositionTailWork;
using Guard = Adapter::PositionTailGuard;
using Clock = std::chrono::steady_clock;

const char* reason_name(Guard reason) {
    switch (reason) {
    case Guard::None: return "none";
    case Guard::Inactive: return "inactive";
    case Guard::Invalidated: return "invalidated";
    case Guard::PublicationRewind: return "publication-rewind";
    case Guard::PublicationGap: return "publication-gap";
    case Guard::BeforeBarrier: return "before-barrier";
    case Guard::NonmonotoneTime: return "nonmonotone-time";
    case Guard::MissingOrigin: return "missing-origin";
    case Guard::ReactivatedOrigin: return "reactivated-origin";
    case Guard::SideMismatch: return "side-mismatch";
    case Guard::InvalidQuantity: return "nonfinite-or-negative-quantity";
    case Guard::QuantumShift: return "quantum-shift-limit";
    case Guard::MagnitudeShift: return "magnitude-shift-limit";
    case Guard::MagnitudeOverflow: return "magnitude-overflow";
    case Guard::InvalidCertificate: return "invalid-certificate";
    case Guard::NonfiniteResult: return "nonfinite-result";
    case Guard::NonPineHost: return "non-pine-host";
    case Guard::ShortSeed: return "short-seed";
    case Guard::Amendment: return "amendment";
    case Guard::Relabel: return "relabel";
    case Guard::Flat: return "flat";
    case Guard::Rebind: return "rebind";
    case Guard::Count: break;
    }
    return "unknown";
}

// This list only names measured fields: no population-derived counts.
#define PF_TAIL_WORK_FIELDS(visit) \
    visit(queries) visit(certified_queries) visit(fallback_queries) \
    visit(update_rows) visit(replay_rows) visit(origin_visits) \
    visit(host_rows) visit(spill_rows) visit(replay_host_rows) \
    visit(begin_updates) visit(begin_rows) visit(closed_updates) \
    visit(open_updates) visit(reissue_updates) visit(reissue_rows) \
    visit(sync_updates) visit(invalidations)

struct Reporter {
    Clock::time_point started = Clock::now();
    Work previous{}, totals{}, query_work{};
    const void* previous_adapter = nullptr;
    std::uint64_t run = 0, run_query = 0;
    bool have_query = false, certified = false;
    Guard reason = Guard::None;

    void sample(const Work& work, const void* adapter, bool query) {
        if (adapter != previous_adapter) previous = {};
        Work delta{};
#define PF_TAIL_WORK_DELTA(field) \
        if (work.field < previous.field) \
            throw std::runtime_error("R11 diagnostic counter decreased within a run"); \
        delta.field = work.field - previous.field; \
        totals.field += delta.field;
        PF_TAIL_WORK_FIELDS(PF_TAIL_WORK_DELTA)
#undef PF_TAIL_WORK_DELTA
        for (std::size_t i = 0; i < static_cast<std::size_t>(Guard::Count); ++i) {
            if (work.guard_reasons[i] < previous.guard_reasons[i])
                throw std::runtime_error("R11 diagnostic guard count decreased within a run");
            totals.guard_reasons[i] += work.guard_reasons[i] - previous.guard_reasons[i];
        }
        previous = work;
        previous_adapter = adapter;
        run_query = work.queries;
        if (query) {
            query_work = delta;
            have_query = true;
            reason = work.guard_reason;
            certified = delta.certified_queries != 0;
        }
    }

    void print(const char* phase) const {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started).count();
        std::fprintf(stderr,
            "PF_TAIL_WORK phase=%s run=%" PRIu64 " query=%" PRIu64
            " run_query=%" PRIu64 " elapsed_ms=%lld"
            " rows_visited=%" PRIu64 " origins_walked=%" PRIu64
            " guard=%s reason=%s certified=%" PRIu64 " fallback=%" PRIu64
            " source_host=%" PRIu64 " source_spill=%" PRIu64 " replay_host=%" PRIu64
            " update_rows=%" PRIu64 " begin_updates=%" PRIu64 " begin_rows=%" PRIu64
            " closed_updates=%" PRIu64 " open_updates=%" PRIu64
            " reissue_updates=%" PRIu64 " reissue_rows=%" PRIu64
            " sync_updates=%" PRIu64 " invalidations=%" PRIu64
            " q_rows=%" PRIu64 " q_origins=%" PRIu64
            " q_host=%" PRIu64 " q_spill=%" PRIu64
            " q_closed=%" PRIu64 " q_open=%" PRIu64 " q_reissue=%" PRIu64,
            phase, run, totals.queries, run_query, static_cast<long long>(elapsed),
            totals.replay_rows, totals.origin_visits,
            have_query ? (certified ? "certified" : "fallback") : "unqueried",
            reason_name(reason), totals.certified_queries, totals.fallback_queries,
            totals.host_rows, totals.spill_rows, totals.replay_host_rows,
            totals.update_rows, totals.begin_updates, totals.begin_rows,
            totals.closed_updates, totals.open_updates, totals.reissue_updates, totals.reissue_rows,
            totals.sync_updates, totals.invalidations,
            query_work.replay_rows, query_work.origin_visits,
            query_work.host_rows, query_work.spill_rows,
            query_work.closed_updates, query_work.open_updates, query_work.reissue_updates);
        for (std::size_t i = 0; i < static_cast<std::size_t>(Guard::Count); ++i)
            std::fprintf(stderr, " guard_%s=%" PRIu64,
                         reason_name(static_cast<Guard>(i)), totals.guard_reasons[i]);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }

    void observe(const Work& work, const void* adapter, bool fast) {
        std::uint64_t verdicts = 0;
        for (auto count : work.guard_reasons) verdicts += count;
        if (work.host_rows != work.update_rows + work.replay_host_rows
            || work.replay_rows != work.replay_host_rows + work.spill_rows
            || work.update_rows != work.begin_rows + work.closed_updates
            || verdicts != work.queries || fast != (work.guard_reason == Guard::None))
            throw std::runtime_error("R11 diagnostic counts do not reconcile");
        sample(work, adapter, true);
        if (work.queries == 1 || totals.queries % 4096 == 0) print("query");
    }

    void phase(const char* phase, const Work& work, const void* adapter) {
        const bool begin = std::strcmp(phase, "reference-run-begin") == 0
            || std::strcmp(phase, "streamed-run-begin") == 0;
        if (begin) {
            // run() resets the summary after this marker, including on host reuse.
            previous = {};
            previous_adapter = adapter;
            run_query = 0;
            have_query = false;
            reason = Guard::None;
            query_work = {};
            ++run;
        } else {
            sample(work, adapter, false);
        }
        print(phase);
    }
};

#undef PF_TAIL_WORK_FIELDS
Reporter* active_reporter = nullptr;
} // namespace r11_tail_work

// Use the same real-adapter observation seam as the independent R11 oracle.
// Enabled in release too; there are no fixture-size or quantity predicates.
#define PINEFORGE_POSITION_TAIL_WORK_REPORT(work, adapter, fast) \
    ::r11_tail_work::active_reporter->observe(work, adapter, fast)
#define PINEFORGE_POSITION_TAIL_WORK_PHASE(phase_name, work, adapter) \
    ::r11_tail_work::active_reporter->phase(phase_name, work, adapter)
#define PINEFORGE_POSITION_TAIL_SCALE_MAIN r11_instrumented_scale_main
#include "test_position_tail_summary_scale.cpp"
#undef PINEFORGE_POSITION_TAIL_SCALE_MAIN
#undef PINEFORGE_POSITION_TAIL_WORK_PHASE
#undef PINEFORGE_POSITION_TAIL_WORK_REPORT
#undef PINEFORGE_POSITION_TAIL_DIAGNOSTICS

#ifdef PINEFORGE_SCRATCH_SEQUENCE_COUNTERS
namespace {
void print_sequence_counters() {
    auto totals = pineforge::detail::scratch_sequence_counter_totals();
    // Also print zero for a compiled site that this fixture did not exercise.
    constexpr const char* tags[] = {"trade_exit_phase_", "permuted", "values", "other"};
    for (const char* tag : tags) {
        auto counters = pineforge::detail::scratch_sequence_tag_total(totals, tag);
        std::sort(counters.last_switches.begin(), counters.last_switches.end(),
            [](const auto& a, const auto& b) { return a.serial < b.serial; });
        std::fprintf(stderr,
            "seq-counters tag=%s flush_writes=%" PRIu64 " flush_bytes=%" PRIu64
            " loads=%" PRIu64 " load_bytes=%" PRIu64 " page_switches=%" PRIu64
            " last_switches=",
            tag, counters.flush_writes, counters.flush_bytes, counters.loads,
            counters.load_bytes, counters.page_switches);
        bool first = true;
        for (const auto& change : counters.last_switches) {
            if (change.serial == 0) continue;
            std::fprintf(stderr, "%s%zu->%zu", first ? "" : ",", change.from, change.to);
            first = false;
        }
        if (first) std::fputs("none", stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }
}
} // namespace
#endif

int main(int argc, char** argv) {
    if (argc != 1) {
        std::fprintf(stderr, "usage: test_position_tail_scale_out_100k\n");
        return 1;
    }
    (void)argv;
    r11_tail_work::Reporter reporter;
    r11_tail_work::active_reporter = &reporter;
    const int result = r11_instrumented_scale_main();
    reporter.print(result == 0 ? "end" : "failed");
    r11_tail_work::active_reporter = nullptr;
#ifdef PINEFORGE_SCRATCH_SEQUENCE_COUNTERS
    print_sequence_counters();
#endif
    return result;
}
