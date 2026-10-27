// RED-first gap ruling: a table sized to the 65,536-row floor must refuse
// admission of row 65,537 before any index bytes or broker state can change.
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
constexpr std::size_t kRows = pineforge::detail::kMaxResidentCohorts;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Cohorts { friend auto access(Cohorts); };
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

class SizedGapHost final : public pineforge::source::PineStrategyHost {
public:
    SizedGapHost() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 5000;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");

        // The sized-limit entry point is R/2. Requests below the floor clamp
        // to N=65,536, so this descriptor deliberately reserves exactly N.
        auto manifest = pineforge::detail::default_scratch_manifest(kRows);
        manifest.U = kRows + 1;
        manifest.B_U = static_cast<std::uint64_t>(kRows + 1) * 32;
        pf_scratch_tables_v1 descriptor{};
        descriptor.struct_size = static_cast<std::uint32_t>(sizeof descriptor);
        descriptor.version = 1;
        descriptor.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
        descriptor.K_q = manifest.K_q; descriptor.B_q = manifest.B_q;
        descriptor.K_e = manifest.K_e; descriptor.B_e = manifest.B_e;
        descriptor.K_c = manifest.K_c; descriptor.B_c = manifest.B_c;
        descriptor.U = manifest.U; descriptor.B_U = manifest.B_U;
        descriptor.V = manifest.V; descriptor.B_V = manifest.B_V;
        descriptor.R = manifest.R;
        require(set_scratch_tables_v1(&descriptor) == 0,
                "select sized bounded cohort table");
    }

    void on_source_bar(const pineforge::Bar&) override {
        const auto i = static_cast<std::size_t>(pine_bar_index());
        const auto& table = adapter_.*access(Cohorts{});
        if (i == kRows) {
            require(table.size() == kRows && table.resident_size() == kRows,
                    "the gap witness must hold exactly N live ids first");
            witness_refusal();
            return;
        }

        const auto na = std::numeric_limits<double>::quiet_NaN();
        strategy_entry("gap-" + std::to_string(i), true, 1.0, na, 1.0);
        max_resident = std::max(max_resident, table.resident_size());
    }

    std::size_t table_size() const {
        return static_cast<std::size_t>((adapter_.*access(Cohorts{})).size());
    }
    std::size_t resident_size() const {
        return (adapter_.*access(Cohorts{})).resident_size();
    }

    bool refused = false;
    std::size_t max_resident = 0;

private:
    void witness_refusal() {
        const auto& table = adapter_.*access(Cohorts{});
        const auto slab = scratch_tables_for_run();
        const auto& region = slab->region(pineforge::detail::ScratchResource::cohort);
        const auto extent = region.logical_extent;
        std::vector<unsigned char> before(static_cast<std::size_t>(extent));
        slab->read_at(region, 0, before.data(), before.size());
        const auto before_hash = broker_state_hash_projection();

        try {
            const auto na = std::numeric_limits<double>::quiet_NaN();
            strategy_entry("gap-overflow", true, 1.0, na, 1.0);
        } catch (const pineforge::detail::ScratchCapacityError& error) {
            refused = true;
            const auto failure = pineforge::classify_run_failure(error);
            require(failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded
                        && failure.args
                        && *failure.args == "{\"resource\":\"cohort\"}",
                    "sized gap refusal must be typed as cohort capacity");
            const std::string message = error.what();
            const auto prefix = std::string("more than ")
                + pineforge::detail::scratch_limit_number(kRows)
                + " order ids active at once";
            const auto count = std::string("live count ")
                + pineforge::detail::scratch_limit_number(kRows + 1);
            const auto cap = std::string("cap ")
                + pineforge::detail::scratch_limit_number(
                    pineforge::detail::kMaxResidentCohortsCap);
            require(message.find(prefix) != std::string::npos
                        && message.find(count) != std::string::npos
                        && message.find(cap) != std::string::npos
                        && message.find("declare a higher freshPerBar") != std::string::npos,
                    "sized gap refusal must name rows, live count, cap and hint");
        }

        require(refused && table.size() == kRows && table.resident_size() == kRows
                    && broker_state_hash_projection() == before_hash,
                "sized gap refusal must preserve rows and broker state hash");
        std::vector<unsigned char> after(before.size());
        slab->read_at(region, 0, after.data(), after.size());
        require(before == after && region.logical_extent == extent,
                "sized gap refusal must preserve index bytes");
    }
};

std::vector<pineforge::Bar> chart(std::size_t count) {
    std::vector<pineforge::Bar> bars(count);
    for (std::size_t i = 0; i < count; ++i)
        bars[i] = {100.0, 101.0, 99.0, 100.0, 1000.0,
                   1'736'121'600'000LL + static_cast<std::int64_t>(i) * 60000};
    return bars;
}

void exact_floor_passes() {
    const auto bars = chart(kRows);
    SizedGapHost host;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_error().empty(), "exactly N live ids must pass");
    require(host.table_size() == kRows && host.resident_size() == kRows
                && host.max_resident == kRows,
            "the exact floor run must retain all N resident ids");
}

void sized_gap_refuses_before_mutation() {
    const auto bars = chart(kRows + 1);
    SizedGapHost host;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_error().empty(), "the test witness must catch the typed refusal");
    require(host.refused && host.table_size() == kRows && host.resident_size() == kRows,
            "N+1 must refuse without dropping or adding an id");
}
} // namespace

int main() {
    try {
        exact_floor_passes();
        sized_gap_refuses_before_mutation();
        std::puts("resident cohort sized gap: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
