// CR-365 R2-A: a new source id refused at the live cohort limit.
//
// The canonical PineExecutionAdapter::cohort_for must refuse such an id before
// the host issues a cohort handle or receipt. The limit, the live set, the
// refusal text and the typed resource are read from the real table and from the
// source constants, never copied: the live set is built from
// detail::kMaxResidentCohorts real pending entries, the expected refusal is the
// one the table's own read-only ticket raises for the same state, and the
// handle witness is the largest handle the kernel really issued to a live row.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/detail/scratch_defaults.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Adapter = pineforge::source::PineExecutionAdapter;
namespace detail = pineforge::detail;

// The same explicit-instantiation access seam as test_id_adapter_storage.cpp:
// no changed class definition or production friend.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Cohorts { friend auto access(Cohorts); };
struct CohortFor { friend auto access(CohortFor); };
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;
template struct Access<CohortFor, &Adapter::cohort_for>;

struct Refusal {
    bool refused = false;
    std::string what, args;
    int code = -1;
};

template<class Call> Refusal attempt(Call&& call) {
    Refusal result;
    try {
        call();
    } catch (const detail::ScratchCapacityError& error) {
        const auto failure = pineforge::classify_run_failure(error);
        result.refused = true;
        result.what = error.what();
        result.args = failure.args ? *failure.args : std::string("<none>");
        result.code = static_cast<int>(failure.code);
    }
    return result;
}

class LiveLimitHost final : public pineforge::source::PineStrategyHost {
public:
    LiveLimitHost() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 5000;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        pf_scratch_tables_v1 descriptor{};
        descriptor.struct_size = static_cast<std::uint32_t>(sizeof descriptor);
        descriptor.version = 1;
        descriptor.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
        if (set_scratch_tables_v1(&descriptor) != 0)
            fail("SETUP: the bounded cohort table was not selected");
    }

    void on_source_bar(const pineforge::Bar&) override {
        const auto bar = static_cast<std::size_t>(pine_bar_index());
        const auto na = std::numeric_limits<double>::quiet_NaN();
        if (bar < detail::kMaxResidentCohorts) {
            strategy_entry("live-" + std::to_string(bar), true, 1.0, na, 1.0); // Never fills.
        } else if (bar == detail::kMaxResidentCohorts) {
            probe();
        }
    }

    bool probed = false;
    std::string verdict;

private:
    void fail(const std::string& why) {
        if (verdict.empty()) verdict = why;
    }

    void probe() {
        probed = true;
        auto& table = adapter_.*access(Cohorts{});
        const std::size_t limit = detail::kMaxResidentCohorts;
        std::size_t live = 0;
        std::uint64_t max_handle = 0;
        for (const auto& row : table.resident_rows()) {
            if (!row.second.spillable()) ++live;
            max_handle = std::max<std::uint64_t>(max_handle, row.second.handle.value);
        }
        const auto logical = table.size();
        const auto resident = table.resident_size();
        // The ticket the table itself raises for another new id in this state.
        const auto reference = attempt([&] { table.require_insert("cr365-live-limit-reference"); });
        const auto hash_before = broker_state_hash_projection();
        const auto actual = attempt([&] { (void)(adapter_.*access(CohortFor{}))("cr365-live-limit-new-id"); });
        const auto hash_after = broker_state_hash_projection();
        const auto next = cohort_open();
        std::printf("R2-A live_limit=%zu live=%zu resident=%zu logical=%llu max_handle=%llu "
                    "next_handle=%llu reference_refused=%d actual_refused=%d resource=%s code=%d "
                    "text=\"%s\"\n",
                    limit, live, resident, static_cast<unsigned long long>(logical),
                    static_cast<unsigned long long>(max_handle),
                    static_cast<unsigned long long>(next.value), reference.refused ? 1 : 0,
                    actual.refused ? 1 : 0, actual.args.c_str(), actual.code, actual.what.c_str());
        if (live != limit || resident < limit || logical != live)
            return fail("SETUP: the live cohort set is not at its limit (live=" + std::to_string(live)
                        + " limit=" + std::to_string(limit) + " logical=" + std::to_string(logical) + ")");
        if (!reference.refused || reference.what != detail::scratch_resident_cohort_limit_message(limit))
            return fail("SETUP: the table ticket does not refuse with the live-limit text derived from the constant");
        if (!actual.refused)
            return fail("BEHAVIOR: cohort_for accepted a new id at the live cohort limit");
        if (actual.what != reference.what || actual.args != reference.args || actual.code != reference.code)
            return fail("BEHAVIOR: cohort_for refused with a different resource or text than the table limit ("
                        + actual.args + " / " + actual.what + ")");
        if (hash_after != hash_before || table.size() != logical || table.resident_size() != resident)
            return fail("BEHAVIOR: the refused cohort_for mutated source or kernel state");
        if (next.value != max_handle + 1)
            return fail("BEHAVIOR: the refused cohort_for consumed a host cohort handle (next="
                        + std::to_string(next.value) + " expected=" + std::to_string(max_handle + 1) + ")");
    }
};
} // namespace

int main() {
    try {
        std::vector<pineforge::Bar> bars(detail::kMaxResidentCohorts + 1);
        for (std::size_t i = 0; i < bars.size(); ++i)
            bars[i] = {100.0, 101.0, 99.0, 100.0, 1000.0,
                       1736121600000LL + static_cast<std::int64_t>(i) * 60000};
        LiveLimitHost host;
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        if (!host.verdict.empty() && !host.probed) throw std::runtime_error(host.verdict);
        if (!host.last_error().empty()) throw std::runtime_error("SETUP: run failed: " + host.last_error());
        if (!host.probed) throw std::runtime_error("SETUP: the probe bar was never reached");
        if (!host.verdict.empty()) throw std::runtime_error(host.verdict);
        std::puts("cohort_for live-limit new id: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
