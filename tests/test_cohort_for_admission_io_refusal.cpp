// CR-365 R2-A2 (extra to the brief): an admission refusal that the read-only
// live-limit ticket cannot see must still precede the host cohort handle.
//
// cohort_for opens the table's backing file during admission. When that open is
// refused (here: the scratch directory is a regular file) the new id must be
// refused before require_host().cohort_open(), so no kernel cohort handle or
// receipt is consumed. The witness is dynamic: it reads the kernel's next
// handle before and after the refused call, and it first proves, through the
// table's own emplace and without touching the kernel, that the injected fault
// really refuses placement. Both facts are read from the real objects.
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
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

class IoRefusalHost final : public pineforge::source::PineStrategyHost {
public:
    IoRefusalHost() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 5000;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
    }

    void on_source_bar(const pineforge::Bar&) override {
        if (pine_bar_index() == 0) probe();
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
        // A regular file is not a directory: a scratch child of it cannot be created.
        table.set_directory("/dev/null/pineforge-cr365-r2-refused");
        // Setup proof, independent of cohort_for and of the kernel: the table
        // itself refuses to place a row, and stays empty.
        using Facts = std::decay_t<decltype(table.find(std::string())->second)>;
        bool setup_refused = false;
        try {
            table.emplace("cr365-setup-probe", Facts{});
        } catch (const std::system_error&) {
            setup_refused = true;
        }
        const auto logical = table.size();
        const auto first = cohort_open();
        const auto hash_before = broker_state_hash_projection();
        bool refused = false;
        std::string what;
        try {
            (void)(adapter_.*access(CohortFor{}))("cr365-admission-io-refused");
        } catch (const std::system_error& error) {
            refused = true;
            what = error.what();
        }
        const auto hash_after = broker_state_hash_projection();
        const auto second = cohort_open();
        std::printf("R2-A2 setup_refused=%d logical=%llu first_handle=%llu second_handle=%llu "
                    "refused=%d same_hash=%d text=\"%s\"\n",
                    setup_refused ? 1 : 0, static_cast<unsigned long long>(logical),
                    static_cast<unsigned long long>(first.value),
                    static_cast<unsigned long long>(second.value), refused ? 1 : 0,
                    hash_after == hash_before ? 1 : 0, what.c_str());
        if (!setup_refused || logical != 0)
            return fail("SETUP: the injected scratch fault did not refuse a table placement");
        if (!refused)
            return fail("BEHAVIOR: cohort_for accepted a new id although the table refuses placement");
        if (table.size() != logical)
            return fail("BEHAVIOR: the refused cohort_for left a source row behind");
        if (second.value != first.value + 1)
            return fail("BEHAVIOR: the refused cohort_for consumed a host cohort handle (next="
                        + std::to_string(second.value) + " expected="
                        + std::to_string(first.value + 1) + ")");
        if (hash_after != hash_before)
            return fail("BEHAVIOR: the refused cohort_for changed the broker state");
    }
};
} // namespace

int main() {
    try {
        std::vector<pineforge::Bar> bars(4);
        for (std::size_t i = 0; i < bars.size(); ++i)
            bars[i] = {100.0, 101.0, 99.0, 100.0, 1000.0,
                       1736121600000LL + static_cast<std::int64_t>(i) * 60000};
        IoRefusalHost host;
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        if (!host.probed) throw std::runtime_error("SETUP: the probe bar was never reached");
        if (!host.verdict.empty()) throw std::runtime_error(host.verdict);
        std::puts("cohort_for admission io refusal: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
