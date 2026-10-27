// Expected red on f66318a3: never-filled entries hit the cap at bar 4,097.
// Uses DenseHost's jailed descriptor, chart, sizing and limit-entry idioms.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/detail/scratch_defaults.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Adapter = pineforge::source::PineExecutionAdapter;
constexpr std::size_t kExpectedLimit = 65536;

// Existing adapter-storage explicit-instantiation seam; no production friend.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Cohorts { friend auto access(Cohorts); };
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

class CeilingHost final : public pineforge::source::PineStrategyHost {
public:
    CeilingHost() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 5000;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");

        const auto manifest = pineforge::detail::default_scratch_manifest(kExpectedLimit + 1);
        pf_scratch_tables_v1 descriptor{};
        descriptor.struct_size = static_cast<std::uint32_t>(sizeof descriptor);
        descriptor.version = 1;
        descriptor.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
        descriptor.K_q = manifest.K_q; descriptor.B_q = manifest.B_q;
        descriptor.K_e = manifest.K_e; descriptor.B_e = manifest.B_e;
        descriptor.K_c = manifest.K_c; descriptor.B_c = manifest.B_c;
        // Keep historical admission above the live cap to isolate its refusal.
        descriptor.U = kExpectedLimit + 1; descriptor.B_U = manifest.B_U;
        descriptor.V = manifest.V; descriptor.B_V = manifest.B_V;
        descriptor.R = manifest.R;
        require(set_scratch_tables_v1(&descriptor) == 0, "select jailed cohort table");
    }

    void on_source_bar(const pineforge::Bar&) override {
        const auto i = pine_bar_index();
        attempted_bar = static_cast<std::size_t>(i) + 1;
        const auto na = std::numeric_limits<double>::quiet_NaN();
        const auto id = "ceiling-" + std::to_string(i);
        strategy_entry(id, true, 1.0, na, 1.0); // Low is 99: this limit never fills.
        ++admitted;
        if (admitted == kExpectedLimit) {
            const auto& table = adapter_.*access(Cohorts{});
            require(table.size() == kExpectedLimit && table.resident_size() == kExpectedLimit
                        && table.held_size() == 0,
                    "65,536 pending origins must stay resident without openings or live units");
            for (const auto& row : table.resident_rows()) {
                require(!row.second.spillable() && row.second.origins.members().size() == 1
                            && row.second.opened.empty() && row.second.live_units_by_origin.empty(),
                        "each fresh id must own one never-filled non-spillable origin");
            }
            const auto heap = adapter_.cohort_heap_estimate();
            std::printf("resident ceiling rows=%zu cohort_row_bytes=%zu "
                        "cohort_heap_estimate=%zu placement_snapshot_bytes=%zu\n",
                        heap.resident_rows, heap.row_bytes, heap.total_bytes,
                        sizeof(pineforge::source::PlacementSnapshot));
            witnessed_ceiling = true;
        }
    }

    std::size_t admitted = 0, attempted_bar = 0;
    bool witnessed_ceiling = false;
};

std::vector<pineforge::Bar> chart(std::size_t count) {
    std::vector<pineforge::Bar> bars(count);
    for (std::size_t i = 0; i < count; ++i)
        bars[i] = {100.0, 101.0, 99.0, 100.0, 1000.0,
                   1736121600000LL + static_cast<std::int64_t>(i) * 60000};
    return bars;
}

void jailed_source_ceiling() {
    const auto bars = chart(kExpectedLimit + 1);
    {
        CeilingHost host;
        host.run(bars.data(), static_cast<int>(kExpectedLimit), "1", "1");
        if (!host.last_error().empty())
            throw std::runtime_error("unexpected jailed refusal at bar "
                + std::to_string(host.attempted_bar) + ": " + host.last_error());
        require(host.admitted == kExpectedLimit && host.witnessed_ceiling && host.trade_count() == 0,
                "all 65,536 fresh limit entries must be admitted and none filled");
    }
    {
        CeilingHost host;
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        require(host.admitted == kExpectedLimit && host.attempted_bar == kExpectedLimit + 1
                    && host.witnessed_ceiling,
                "first jailed refusal must be at bar 65,537");
        require(host.last_error().find("more than 65,536 order ids active at once")
                    != std::string::npos,
                "jailed source refusal must name the new resident limit");
        require(std::string(pineforge::run_failure_code_of(host)) == "scratch_capacity_exceeded"
                    && std::string(pineforge::run_failure_args_of(host)) == "{\"resource\":\"cohort\"}",
                "jailed source refusal must retain the typed cohort resource");
        require(host.trade_count() == 0, "no entry may fill before the refusal");
    }
}
} // namespace

int main() {
    try {
        jailed_source_ceiling();
        std::puts("source resident cohort ceiling: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
