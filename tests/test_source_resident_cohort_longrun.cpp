// RED mutation: in resident_cohort_sizing(), remove `declared &&` from the
// declared_over_cap result. A default-derived 2,000,000-bar demand must not
// refuse this run.
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
constexpr std::size_t kBars = 2'000'000;
constexpr std::size_t kIdCount = 2;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Cohorts { friend auto access(Cohorts); };
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

class LongRunHost final : public pineforge::source::PineStrategyHost {
public:
    LongRunHost() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 5000;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");

        // R/2 is the default-derived bar demand. The bounded backend is
        // deliberately selected so this exercises the jailed binder.
        const auto manifest = pineforge::detail::default_scratch_manifest(kBars);
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
                "select jailed long-run cohort table");
    }

    void on_source_bar(const pineforge::Bar&) override {
        const auto i = static_cast<std::size_t>(pine_bar_index());
        const bool long_side = (i & 1) == 0;
        const std::string id = long_side ? "Long" : "Short";
        if (i != 0) strategy_cancel(long_side ? "Short" : "Long");

        // The limit is below every chart low/high, so the cancelled order is
        // the only live order and the two ids are reused on every bar.
        const auto na = std::numeric_limits<double>::quiet_NaN();
        strategy_entry(id, long_side, 1.0, na, 1.0);

        const auto& table = adapter_.*access(Cohorts{});
        max_logical = std::max(max_logical, static_cast<std::size_t>(table.size()));
        max_resident = std::max(max_resident, table.resident_size());
        require(table.size() <= kIdCount && table.resident_size() <= kIdCount,
                "reused ids must keep the resident cohort table small");
    }

    std::size_t max_logical = 0;
    std::size_t max_resident = 0;
};

std::vector<pineforge::Bar> chart() {
    std::vector<pineforge::Bar> bars(kBars);
    for (std::size_t i = 0; i < kBars; ++i)
        bars[i] = {100.0, 101.0, 99.0, 100.0, 1000.0,
                   1'736'121'600'000LL + static_cast<std::int64_t>(i) * 60000};
    return bars;
}

void jailed_reused_id_longrun() {
    const auto bars = chart();
    LongRunHost host;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_error().empty(), "default-derived long run was refused");
    require(host.max_logical <= kIdCount && host.max_resident <= kIdCount,
            "two reused ids must keep logical and resident cohorts bounded");
    require(host.trade_count() == 0, "the below-range limit orders must not fill");
}
} // namespace

int main() {
    try {
        jailed_reused_id_longrun();
        std::puts("resident cohort reused-id long run: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
