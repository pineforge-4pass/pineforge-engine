#include <pineforge/detail/scratch_slab.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr int kBars = 5000;
constexpr int kOldLimit = 4096;
constexpr double kClose = 100.0;
constexpr double kLimit = 1.0; // close * 0.01; every fixture low is above it.

class ManyRestingOrders final : public pineforge::source::PineStrategyHost {
public:
    ManyRestingOrders() {
        configure_pine_strategy(pineforge::source::PineStrategyConfig{});
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        const int i = pine_bar_index();
        if (i != attempted)
            throw std::runtime_error("expected bar index " + std::to_string(attempted)
                                     + ", got " + std::to_string(i));
        if (i == kOldLimit) {
            pending_at_old_limit = pending_order_count();
            const auto& slab = scratch_tables_for_run();
            if (slab) {
                cohort_extent = slab->region(pineforge::detail::ScratchResource::cohort)
                                    .logical_extent;
            }
        }
        if (i < kBars) {
            ++attempted;
            strategy_order("o" + std::to_string(i), true, 1.0, bar.close * 0.01);
            ++returned;
        }
    }

    int attempted = 0, returned = 0, pending_at_old_limit = -1;
    std::uint64_t cohort_extent = 0;
};

int compare(const std::vector<pineforge::Bar>& bars) {
    if (bars.size() != kBars) {
        std::fprintf(stderr, "NOT COMPARED: input length: expected_bars=%d actual_bars=%zu\n",
                     kBars, bars.size());
        return 1;
    }
    for (std::size_t i = 0; i < bars.size(); ++i) {
        if (bars[i].close != kClose || !(bars[i].low > kLimit)) {
            std::fprintf(stderr, "NOT COMPARED: resting-order input at bar %zu: "
                         "expected_close=%.2f actual_close=%.2f expected_low>%.2f actual_low=%.2f\n",
                         i, kClose, bars[i].close, kLimit, bars[i].low);
            return 1;
        }
    }

    ManyRestingOrders host;
    const auto state = static_cast<pineforge::BacktestEngine*>(&host);
    pf_scratch_tables_v1 config{};
    config.struct_size = static_cast<std::uint32_t>(sizeof config);
    config.version = 1;
    config.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
    const int setup = strategy_set_scratch_tables_v1(state, &config);
    if (setup != 0) {
        std::fprintf(stderr, "NOT COMPARED: bounded scratch selection: expected_status=0 actual_status=%d\n",
                     setup);
        return 1;
    }
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    if (host.pending_at_old_limit != kOldLimit || host.cohort_extent == 0) {
        std::fprintf(stderr, "NOT COMPARED: bounded cohort book before bar %d's order: "
                     "expected_pending=%d actual_pending=%d expected_cohort_extent>0 actual_cohort_extent=%llu\n",
                     kOldLimit, kOldLimit, host.pending_at_old_limit,
                     static_cast<unsigned long long>(host.cohort_extent));
        if (!host.last_error().empty()) std::fprintf(stderr, "%s\n", host.last_error().c_str());
        return 1;
    }

    const std::string_view code = strategy_get_last_error_code(state);
    const std::string_view args = strategy_get_last_error_args(state);
    if (host.last_run_status() == 1 && code == "scratch_capacity_exceeded"
        && args == R"({"resource":"cohort_orders_live"})"
        && host.attempted == kOldLimit + 1 && host.returned == kOldLimit) {
        std::fprintf(stderr, "FAIL: bar %d: expected %d pending orders; %s %s after %d placements\n",
                     kOldLimit, kBars, code.data(), args.data(), host.returned);
        return 1;
    }
    const bool completed = host.native_state().kind == pineforge::NativeLifecycleKind::Completed;
    if (host.last_run_status() != 0 || !code.empty() || !args.empty() || !host.last_error().empty() || !completed
        || host.attempted != kBars || host.returned != kBars) {
        std::fprintf(stderr, "NOT COMPARED: run did not complete the input: "
                     "expected_status=0 actual_status=%d expected_completed=1 actual_completed=%d "
                     "expected_placements=%d attempted=%d returned=%d expected_error=<empty> "
                     "actual_code=%s actual_args=%s actual_error=%s\n", host.last_run_status(), completed,
                     kBars, host.attempted, host.returned, code.data(), args.data(), host.last_error().c_str());
        return 1;
    }

    const int pending = host.pending_order_count();
    const int trades = host.report_trade_count();
    if (pending != kBars || trades != 0) {
        std::fprintf(stderr, "FAIL: expected_pending=%d actual_pending=%d expected_trades=0 actual_trades=%d\n",
                     kBars, pending, trades);
        return 1;
    }
    // Validate the full set without relying on the projection's display order.
    std::vector<bool> seen(kBars);
    for (int i = 0; i < pending; ++i) {
        pf_pending_order_v1_t row{};
        const int copied = strategy_pending_order_get(state, i, &row, sizeof row);
        if (copied != 0) {
            std::fprintf(stderr, "NOT COMPARED: pending row %d: expected_copy=0 actual_copy=%d\n",
                         i, copied);
            return 1;
        }
        const std::string_view id = row.id;
        int bar = -1;
        const auto* end = id.data() + id.size();
        const auto parsed = std::from_chars(id.data() + (id.empty() ? 0 : 1), end, bar);
        if (id.empty() || id.front() != 'o' || parsed.ec != std::errc{} || parsed.ptr != end
            || bar < 0 || bar >= kBars || seen[bar] || id != "o" + std::to_string(bar)) {
            std::fprintf(stderr, "FAIL: pending row %d: expected a distinct ID in o0..o%d; actual_id=%s\n",
                         i, kBars - 1, row.id);
            return 1;
        }
        seen[bar] = true;
        if (row.is_long != 1 || row.qty != 1.0 || row.limit_price != kLimit) {
            std::fprintf(stderr, "FAIL: pending row %d (%s): "
                         "expected_long=1 actual_long=%u expected_qty=1 actual_qty=%g "
                         "expected_limit=%g actual_limit=%g\n", i, row.id,
                         static_cast<unsigned>(row.is_long), row.qty, kLimit, row.limit_price);
            return 1;
        }
    }
    std::printf("PASS: bounded scratch kept %d distinct orders pending; trades=%d\n", pending, trades);
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--empty") == 0) return compare({});
        const bool wrong_input = argc == 2 && std::strcmp(argv[1], "--wrong-input") == 0;
        if (argc != 1 && !wrong_input) {
            std::fprintf(stderr, "NOT COMPARED: expected no argument, --empty or --wrong-input; actual_argc=%d\n",
                         argc);
            return 1;
        }
        std::vector<pineforge::Bar> bars(kBars);
        for (int i = 0; i < kBars; ++i)
            bars[i] = {100.0, 101.0, 99.0, kClose, 1000.0, 1736121600000LL + i * 60000LL};
        if (wrong_input) bars[kOldLimit].low = 1.0;
        return compare(bars);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NOT COMPARED: expected no setup/read exception; actual_error=%s\n", error.what());
        return 1;
    }
}
