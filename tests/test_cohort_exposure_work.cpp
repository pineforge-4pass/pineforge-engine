// Observe the real adapter, including its callers, before the static archive
// is searched. Only the existing work macro is overridden.
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/source/pine_cohort_exposure.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace exposure_work_fixture {
struct Site {
    const char* name;
    std::uint64_t calls = 0;
};
struct Work {
    const void* owner = nullptr;
    std::uint64_t run = 0;
    bool have_run = false, identity_ok = true, compact = true, singleton = true;
    std::uint64_t calls = 0, steps = 0, windows = 0, rows = 0;
    std::uint64_t step_events = 0, window_events = 0, row_events = 0;
    std::uint64_t window = 0, max_window = 0, max_rows = 0;
    std::uint64_t path_b = 0, abandoned = 0;
    std::array<Site, 10> sites{{
        {"cohort_exposure_site_short_seed_context_is_live"},
        {"cohort_exposure_site_reconcile_exit_reservations_after_margin"},
        {"cohort_exposure_site_place_entry"},
        {"cohort_exposure_site_withdraw_unplaced_cap_adds"},
        {"cohort_exposure_site_schedule_tv_money_long_margin_before_trail"},
        {"cohort_exposure_site_fill_pooc_close_exits"},
        {"cohort_exposure_site_on_applied_opening"},
        {"cohort_exposure_site_on_applied_held"},
        {"cohort_exposure_site_on_applied_margin_revival"},
        {"cohort_exposure_site_PendingIntentView_copy_v1"}
    }};
};
inline Work* active = nullptr;

void observe(const char* site, const void* owner, std::uint64_t run,
             std::uint64_t count) noexcept {
    if (!active || std::strncmp(site, "cohort_exposure_", 16) != 0) return;
    auto& work = *active;
    work.identity_ok = work.identity_ok && owner == work.owner;
    if (!work.have_run) {
        work.run = run;
        work.have_run = true;
    }
    work.identity_ok = work.identity_ok && run == work.run;
    if (std::strcmp(site, "cohort_exposure_call") == 0) work.calls += count;
    else if (std::strcmp(site, "cohort_exposure_step") == 0) {
        work.steps += count;
        ++work.step_events;
    } else if (std::strcmp(site, "cohort_exposure_window") == 0) {
        work.windows += count;
        work.window = count;
        if (count > work.max_window) work.max_window = count;
        ++work.window_events;
    } else if (std::strcmp(site, "cohort_exposure_rows") == 0) {
        work.rows += count;
        if (count > work.max_rows) work.max_rows = count;
        work.compact = work.compact && work.window <= 4 * count + 16;
        work.singleton = work.singleton && work.window <= 1 && count <= 1;
        ++work.row_events;
    } else if (std::strcmp(site, "cohort_exposure_path_b") == 0) work.path_b += count;
    else if (std::strcmp(site, "cohort_exposure_b_abandoned") == 0) work.abandoned += count;
    else {
        for (auto& group : work.sites)
            if (std::strcmp(site, group.name) == 0) group.calls += count;
    }
}
} // namespace exposure_work_fixture

#define PINEFORGE_LOOKUP_INDEX_WORK(site, owner, run, count) \
    ::exposure_work_fixture::observe(site, owner, run, count)
#include "../src/source/pine_adapter.cpp"
#undef PINEFORGE_LOOKUP_INDEX_WORK

namespace exposure_work_fixture {
using namespace pineforge;
int failures = 0;
#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                           \
    }                                                                         \
} while (false)

constexpr std::array<int, 3> sizes{{128, 256, 512}};
constexpr double na = std::numeric_limits<double>::quiet_NaN();

class OpeningHost final : public source::PineStrategyHost {
public:
    OpeningHost(int entries, bool reused) : entries_(entries), reused_(reused) {
        source::PineStrategyConfig config;
        config.initial_capital = 10000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = entries + 1;
        config.calc_on_order_fills = true;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        // A fill callback may revisit this bar. Issue exactly once per bar.
        if (bar == last_bar_ || bar >= entries_) return;
        last_bar_ = bar;
        strategy_entry(reused_ ? "entry" : "entry-" + std::to_string(bar),
                       true, na, na, 1.0);
        ++issued_;
    }

    const void* adapter_identity() const noexcept { return &adapter_; }
    int issued() const noexcept { return issued_; }

    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        sink.s("exposure-work/openings");
        sink.i(entries_); sink.b(reused_);
        sink.i(last_bar_); sink.i(issued_);
    }

private:
    int entries_, last_bar_ = -1, issued_ = 0;
    bool reused_;
};

Work measure(int entries, bool reused) {
    OpeningHost host(entries, reused);
    std::vector<Bar> bars;
    for (int i = 0; i < entries + 2; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0,
                        1736121600000LL + i * 60000LL});
    Work work;
    work.owner = host.adapter_identity();
    active = &work;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    active = nullptr;
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.issued() == entries);
    CHECK(host.live_position_size() == static_cast<double>(entries));
    CHECK(work.identity_ok && work.have_run);
    CHECK(work.calls > 0 && work.steps > 0);
    CHECK(work.step_events == work.calls);
    CHECK(work.window_events == work.calls && work.row_events == work.calls);
    const char* shape = reused ? "reused" : "distinct";
    std::printf("exposure-work %s %d %llu %llu %llu %llu %llu %llu %llu %llu\n",
        shape, entries, static_cast<unsigned long long>(work.calls),
        static_cast<unsigned long long>(work.steps),
        static_cast<unsigned long long>(work.windows),
        static_cast<unsigned long long>(work.rows),
        static_cast<unsigned long long>(work.max_window),
        static_cast<unsigned long long>(work.max_rows),
        static_cast<unsigned long long>(work.path_b),
        static_cast<unsigned long long>(work.abandoned));
    std::fflush(stdout);
    return work;
}

struct Baseline {
    std::string shape;
    int entries = 0;
    std::uint64_t calls = 0, steps = 0;
};

std::vector<Baseline> read_baseline(const char* path) {
    std::ifstream input(path);
    CHECK(input.is_open());
    std::vector<Baseline> rows;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string marker;
        Baseline row;
        if (fields >> marker && marker == "exposure-work") {
            CHECK(static_cast<bool>(fields >> row.shape >> row.entries
                                           >> row.calls >> row.steps));
            rows.push_back(row);
        }
    }
    CHECK(rows.size() == 6);
    return rows;
}

std::uint64_t legacy_reused_steps(std::uint64_t n) {
    return 2 * n * (n + 1) * (n + 2) / 3;
}

void calibrate(const std::array<Work, 3>& work) {
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const auto expected = legacy_reused_steps(sizes[i]);
        const auto actual = work[i].steps;
        const auto delta = actual > expected ? actual - expected : expected - actual;
        CHECK(actual >= static_cast<std::uint64_t>(sizes[i]) * (sizes[i] + 1) / 2);
        CHECK(delta * 100 <= expected);
        CHECK(work[i].compact);
        std::printf("exposure-calibration %d actual=%llu expected=%llu compact=%d\n",
            sizes[i], static_cast<unsigned long long>(actual),
            static_cast<unsigned long long>(expected), work[i].compact ? 1 : 0);
    }
}

void compare_baseline(const std::vector<Baseline>& baseline,
                      const char* shape, const std::array<Work, 3>& work) {
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        int matches = 0;
        for (const auto& row : baseline) {
            if (row.shape != shape || row.entries != sizes[i]) continue;
            ++matches;
            CHECK(work[i].calls == row.calls);
            CHECK(work[i].steps <= row.steps);
            if (row.shape == "reused") {
                const auto expected = legacy_reused_steps(row.entries);
                const auto delta = row.steps > expected
                    ? row.steps - expected : expected - row.steps;
                CHECK(delta * 100 <= expected);
            }
        }
        CHECK(matches == 1);
    }
}

void reused_growth(const std::array<Work, 3>& work) {
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const std::uint64_t n = sizes[i];
        CHECK(work[i].compact);
        CHECK(work[i].steps >= n * (n + 1) / 2);
        CHECK(work[i].steps <= 8 * n * n);
        if (i) CHECK(work[i].steps <= 5 * work[i - 1].steps);
    }
}

void distinct_guard(const std::array<Work, 3>& work) {
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        // An exposed distinct id has one position and one row; pending
        // or emptied cohorts may have zero. The frozen selector visits
        // a contributing row twice, including its final unsuccessful scan.
        CHECK(work[i].singleton);
        CHECK(work[i].max_window == 1 && work[i].max_rows == 1);
        CHECK(work[i].steps <= 2 * work[i].calls);
        if (i) CHECK(work[i].steps <= 5 * work[i - 1].steps);
    }
}

void history_guard() {
    const auto fold = [](std::size_t reissues) {
        source::OriginRoster origins;
        std::unordered_map<std::uint64_t, double> units{{1, 1.0}};
        origins.push_back({{"history", 1}, 1});
        for (std::size_t i = 0; i < reissues; ++i)
            origins.push_back({{"history", 1}, i + 2});
        CHECK(units.size() == 1);
        CHECK(origins.positions_of(1) && origins.positions_of(1)->size() == 1);
        CHECK(origins.members().size() > 4 * units.size() + 16);
        const auto result = source::detail::fold_cohort_exposure(origins, units);
        CHECK(result.total == 1.0);
        return result;
    };
    const auto small = fold(64);
    const auto large = fold(128);
    CHECK(small.path == 2 && large.path == 2);
    const auto delta = large.steps > small.steps
        ? large.steps - small.steps : small.steps - large.steps;
    CHECK(delta <= 16);
    std::printf("exposure-history 64=%llu 128=%llu paths=%u,%u\n",
        static_cast<unsigned long long>(small.steps),
        static_cast<unsigned long long>(large.steps),
        static_cast<unsigned>(small.path), static_cast<unsigned>(large.path));

    constexpr std::size_t mapped_positions =
        source::detail::kCohortFoldStack + 1;
    const auto overflow_fold = [mapped_positions](std::size_t tail) {
        source::OriginRoster origins;
        std::unordered_map<std::uint64_t, double> units{{1, 1.0}};
        for (std::size_t i = 0; i < mapped_positions; ++i)
            origins.push_back({{"history-overflow", 1}, 1});
        for (std::size_t i = 0; i < tail; ++i) {
            const auto incarnation = mapped_positions + i + 1;
            origins.push_back({{"history-overflow", 1}, incarnation});
        }
        CHECK(origins.positions_of(1)
              && origins.positions_of(1)->size() == mapped_positions);
        CHECK(origins.members().size() > 4 * units.size() + 16);
        const auto result = source::detail::fold_cohort_exposure(origins, units);
        CHECK(result.total == static_cast<double>(mapped_positions));
        return result;
    };
    // This is the selector's safe derived (P + 1) * (R + P) charge
    // at P = 129 and R = 1.
    const auto selector_charge =
        (mapped_positions + 1) * (1 + mapped_positions);
    const auto narrow = overflow_fold(selector_charge);
    const auto wide = overflow_fold(2 * selector_charge);
    CHECK(narrow.path == 3 && wide.path == 3);
    const auto overflow_delta = wide.steps > narrow.steps
        ? wide.steps - narrow.steps : narrow.steps - wide.steps;
    // Both rows have the same P and R; a bounded fallback visits no tail row.
    // Allow the selector's derived mapped-position upper charge, independent
    // of M. The fixed fallback has delta zero.
    const auto derived_bound = static_cast<std::uint64_t>(
        mapped_positions * (mapped_positions + 1));
    CHECK(overflow_delta <= derived_bound);
    std::printf("exposure-history-overflow %llu %llu delta=%llu\n",
        static_cast<unsigned long long>(narrow.steps),
        static_cast<unsigned long long>(wide.steps),
        static_cast<unsigned long long>(overflow_delta));
}

int caller_groups() {
    for (const int entries : {256, 512}) {
        const auto work = measure(entries, true);
        if (failures) return 1;
        std::uint64_t attributed = 0;
        for (const auto& group : work.sites) attributed += group.calls;
        if (!attributed) {
            std::puts("SKIP cohort exposure caller groups: overlay events are absent");
            return 0;
        }
        CHECK(work.sites[7].calls > 0); // Held-cohort loop on opening Applied.
        for (const auto& group : work.sites)
            std::printf("exposure-site reused %d %s %llu\n", entries, group.name,
                        static_cast<unsigned long long>(group.calls));
    }
    return failures == 0 ? 0 : 1;
}
} // namespace exposure_work_fixture

int main(int argc, char** argv) {
    using namespace exposure_work_fixture;
    bool calibration = false;
    bool callers = false;
    const char* baseline_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--calibrate") == 0) calibration = true;
        else if (std::strcmp(argv[i], "--caller-groups") == 0) callers = true;
        else if (std::strcmp(argv[i], "--baseline") == 0 && i + 1 < argc)
            baseline_path = argv[++i];
        else return 2;
    }
    if (callers) {
        if (calibration || baseline_path) return 2;
        return caller_groups();
    }
    if (calibration && baseline_path) return 2;
    std::array<Work, 3> reused, distinct;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        reused[i] = measure(sizes[i], true);
        distinct[i] = measure(sizes[i], false);
    }
    if (calibration) calibrate(reused);
    else reused_growth(reused);
    distinct_guard(distinct);
    history_guard();
    if (baseline_path) {
        const auto baseline = read_baseline(baseline_path);
        compare_baseline(baseline, "reused", reused);
        compare_baseline(baseline, "distinct", distinct);
    }
    return failures == 0 ? 0 : 1;
}
