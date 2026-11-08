// CR-365 R2-B: the work cadence of an existing logical id through cohort_for.
//
// An id that already has a cohort row is answered by the canonical
// PineExecutionAdapter::cohort_for with the read-only ticket and one table
// lookup, so each call costs exactly one implicit spill tick and one row
// dereference. The expected cadence is not a hand-written constant: it is the
// per-call delta of every cohort-table work counter measured on an identical
// run that issues those two table primitives directly. The adapter path must
// reproduce that history call for call, in both the unjailed and the bounded
// cohort profile, at two spill intervals. The reference must itself show spill
// passes whose number follows the interval input, so a counter that ignored
// its input could not satisfy the test.
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Adapter = pineforge::source::PineExecutionAdapter;

// The same explicit-instantiation access seam as test_id_adapter_storage.cpp:
// no changed class definition or production friend.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Cohorts { friend auto access(Cohorts); };
struct CohortFor { friend auto access(CohortFor); };
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;
template struct Access<CohortFor, &Adapter::cohort_for>;

// rows_visited_by_spill, find_resident_probes, resident_rows_visited,
// held_rows_visited, iterated_rows, rows_visited_by_capacity, rows_spilled,
// rows_restored.
using Work = std::array<std::uint64_t, 8>;

class CadenceHost final : public pineforge::source::PineStrategyHost {
public:
    static constexpr std::size_t kIds = 160, kCalls = 40;

    CadenceHost(bool jailed, bool via_adapter, std::size_t interval)
        : via_adapter_(via_adapter), interval_(interval) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 5000;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        if (jailed) {
            pf_scratch_tables_v1 descriptor{};
            descriptor.struct_size = static_cast<std::uint32_t>(sizeof descriptor);
            descriptor.version = 1;
            descriptor.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
            if (set_scratch_tables_v1(&descriptor) != 0)
                verdict = "SETUP: the bounded cohort table was not selected";
        }
    }

    void on_source_bar(const pineforge::Bar&) override {
        const auto bar = static_cast<std::size_t>(pine_bar_index());
        const auto na = std::numeric_limits<double>::quiet_NaN();
        if (bar < kIds) {
            strategy_entry("cad-" + std::to_string(bar), true, 1.0, na, 1.0); // Never fills.
        } else if (bar == kIds) {
            measure();
        }
    }

    std::vector<Work> history;
    std::string id, verdict;
    bool measured = false;

private:
    Work snapshot() const {
        const auto& w = adapter_.cohort_table_work();
        return {w.rows_visited_by_spill, w.find_resident_probes, w.resident_rows_visited,
                w.held_rows_visited, w.iterated_rows, w.rows_visited_by_capacity,
                w.rows_spilled, w.rows_restored};
    }

    void measure() {
        measured = true;
        auto& table = adapter_.*access(Cohorts{});
        adapter_.fixture_cohort_implicit_spill_interval(interval_);
        id = "cad-" + std::to_string(kIds / 2);
        if (!table.contains(id)) {
            verdict = "SETUP: the measured id has no cohort row";
            return;
        }
        for (std::size_t call = 0; call < kCalls; ++call) {
            const auto before = snapshot();
            if (via_adapter_) {
                (void)(adapter_.*access(CohortFor{}))(id);
            } else {
                // The two table primitives an existing id costs, in the same order.
                table.require_insert(id);
                auto found = table.find(id);
                if (found == table.end()) {
                    verdict = "SETUP: the measured id vanished from the table";
                    return;
                }
                const auto handle = found->second.handle;
                (void)handle;
            }
            const auto after = snapshot();
            Work delta{};
            for (std::size_t k = 0; k < delta.size(); ++k) delta[k] = after[k] - before[k];
            history.push_back(delta);
        }
    }

    bool via_adapter_;
    std::size_t interval_;
};

struct Observation {
    std::vector<Work> history;
    std::string id;
};

Observation observe(bool jailed, bool via_adapter, std::size_t interval) {
    std::vector<pineforge::Bar> bars(CadenceHost::kIds + 1);
    for (std::size_t i = 0; i < bars.size(); ++i)
        bars[i] = {100.0, 101.0, 99.0, 100.0, 1000.0,
                   1736121600000LL + static_cast<std::int64_t>(i) * 60000};
    CadenceHost host(jailed, via_adapter, interval);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    if (!host.last_error().empty()) throw std::runtime_error("SETUP: run failed: " + host.last_error());
    if (!host.measured) throw std::runtime_error("SETUP: the measurement bar was never reached");
    if (!host.verdict.empty()) throw std::runtime_error(host.verdict);
    if (host.history.size() != CadenceHost::kCalls) throw std::runtime_error("SETUP: short call history");
    return {host.history, host.id};
}

std::size_t spill_passes(const std::vector<Work>& history) {
    std::size_t passes = 0;
    for (const auto& delta : history) passes += delta[0] != 0 ? 1 : 0;
    return passes;
}

std::string describe(const Work& delta) {
    std::string text = "[";
    for (std::size_t k = 0; k < delta.size(); ++k)
        text += (k ? "," : "") + std::to_string(delta[k]);
    return text + "]";
}

void report(bool jailed, std::size_t interval, const char* path, const Observation& seen) {
    Work total{};
    std::string visits;
    for (const auto& delta : seen.history) {
        for (std::size_t k = 0; k < total.size(); ++k) total[k] += delta[k];
        visits += (visits.empty() ? "" : ",") + std::to_string(delta[0]);
    }
    std::printf("R2-B profile=%s interval=%zu path=%s id=%s calls=%zu spill_passes=%zu "
                "total=%s spill_visits_per_call=[%s]\n",
                jailed ? "bounded" : "unjailed", interval, path, seen.id.c_str(),
                seen.history.size(), spill_passes(seen.history), describe(total).c_str(),
                visits.c_str());
}
} // namespace

int main() {
    try {
        for (const bool jailed : {false, true}) {
            std::size_t passes_by_interval[2]{};
            std::size_t index = 0;
            for (const std::size_t interval : {std::size_t{4}, std::size_t{8}}) {
                const auto reference = observe(jailed, false, interval);
                const auto adapter = observe(jailed, true, interval);
                report(jailed, interval, "reference", reference);
                report(jailed, interval, "adapter", adapter);
                const auto passes = spill_passes(reference.history);
                if (passes == 0)
                    throw std::runtime_error("SETUP: the reference shows no spill pass, so the cadence is unobserved");
                passes_by_interval[index++] = passes;
                for (std::size_t call = 0; call < reference.history.size(); ++call) {
                    if (adapter.history[call] != reference.history[call])
                        throw std::runtime_error(std::string("BEHAVIOR: existing-id cadence changed in the ")
                            + (jailed ? "bounded" : "unjailed") + " profile at interval "
                            + std::to_string(interval) + ", call " + std::to_string(call)
                            + ": adapter " + describe(adapter.history[call])
                            + " reference " + describe(reference.history[call]));
                }
            }
            if (passes_by_interval[0] <= passes_by_interval[1])
                throw std::runtime_error("SETUP: the spill pass count does not follow the interval input");
        }
        std::puts("cohort_for existing id cadence: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
