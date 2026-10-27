// CR-421: measurement probe, written before any complexity fix.
// Run red/green on the lab only; wall times and ratios are observations,
// never pass/fail thresholds. No product TU or mutation seam is included.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/native_execution_consumer.hpp"

#include <sys/resource.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>

namespace dormant_io {
// Counter-only part of test_native_dormant_book_cost's dormant_io observer.
// Every call reaches the real syscall; no result/errno/data is synthesized.
inline std::size_t preads = 0;
}

extern "C" ssize_t pread(int fd, void* data, std::size_t bytes, off_t offset) {
    ++dormant_io::preads;
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, bytes, offset));
}
#endif

namespace {
using namespace pineforge;
constexpr std::int64_t kStart = 1736121600000LL;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct RunWork {
    double wall_ms = kNaN;
    std::uint64_t cohort_rows = 0;
    std::size_t preads = 0;
};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// C3 exposes these read-only diagnostics; BASE does not. Substitution checks
// the actual accessor set so the same TU builds on both, without a base stub.
template<class Consumer>
auto print_dormant(const Consumer& consumer, int)
    -> decltype(consumer.scanned_rows(), consumer.fast_points(),
                consumer.eager_points(), consumer.projected_reads(), void()) {
    std::printf("CR421 mech scanned_rows=%llu fast_points=%llu eager_points=%llu projected_reads=%llu\n",
                static_cast<unsigned long long>(consumer.scanned_rows()),
                static_cast<unsigned long long>(consumer.fast_points()),
                static_cast<unsigned long long>(consumer.eager_points()),
                static_cast<unsigned long long>(consumer.projected_reads()));
}
template<class Consumer>
void print_dormant(const Consumer&, long) {
    std::printf("CR421 mech dormant_counters=unavailable\n");
}

class Host final : public source::PineStrategyHost {
public:
    Host() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000000.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        // Keep fixture defaults: no orders-on-close, no fill recalculation,
        // no hash recording. In particular, do not override the hash fold.
    }

    void on_source_bar(const Bar&) override {
        strategy_order("standing-" + std::to_string(pine_bar_index()),
                       true, 1.0, 1.0);
    }

    void print_mechanism() const {
        const auto& work = adapter_.cohort_table_work();
        std::printf("CR421 mech cohort_spill_rows=%llu cohort_capacity_rows=%llu "
                    "cohort_iterated_rows=%llu cohort_resident_visits=%llu "
                    "cohort_held_visits=%llu cohort_find_probes=%llu "
                    "cohort_spilled=%llu cohort_restored=%llu\n",
                    static_cast<unsigned long long>(work.rows_visited_by_spill),
                    static_cast<unsigned long long>(work.rows_visited_by_capacity),
                    static_cast<unsigned long long>(work.iterated_rows),
                    static_cast<unsigned long long>(work.resident_rows_visited),
                    static_cast<unsigned long long>(work.held_rows_visited),
                    static_cast<unsigned long long>(work.find_resident_probes),
                    static_cast<unsigned long long>(work.rows_spilled),
                    static_cast<unsigned long long>(work.rows_restored));
        const auto& consumer = as_native_consumer(execution_consumer());
        const auto lookups = consumer.request_core().book_lookup_reads();
        std::printf("CR421 mech book_lookup_rows=%llu book_lookup_steps=%llu\n",
                    static_cast<unsigned long long>(lookups.rows),
                    static_cast<unsigned long long>(lookups.steps));
        print_dormant(consumer, 0);
    }

    std::uint64_t cohort_rows() const {
        return adapter_.cohort_table_work().iterated_rows;
    }

    std::uint64_t scanned_rows() const {
        return as_native_consumer(execution_consumer()).scanned_rows();
    }
};

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 100.0, 100.0, 100.0, 1.0,
                        kStart + static_cast<std::int64_t>(i) * 60000LL});
    return bars;
}

RunWork run(int n) {
    Host host;
    const auto bars = tape(n);
#if defined(__linux__)
    const auto reads_before = dormant_io::preads;
#endif
    const auto begin = std::chrono::steady_clock::now();
    // Same public PineStrategyHost entry as generated Pine: prepare_native_begin
    // provisions the adapter, native consumer, run arena and scratch tables.
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    const auto end = std::chrono::steady_clock::now();
#if defined(__linux__)
    const auto reads = dormant_io::preads - reads_before;
#endif
    const double wall_ms = std::chrono::duration<double, std::milli>(end - begin).count();
    const auto lifecycle = host.native_state().kind;
    require(lifecycle == NativeLifecycleKind::Completed,
            "CR421 run did not reach Completed: " + host.last_error());

    // Independent hand oracle: one unique-id buy limit per bar, no cancels;
    // every low is 100 > 1, so none can fill. All N must remain pending,
    // including the last callback's placement; there can be no closed trade.
    // Inspect only after timing: enumerating copy_v1(i) would add its own cost.
    const int orders = host.pending_intent_view().size();
    const int closed = host.trade_count();
    struct rusage usage {};
    const int rss_status = getrusage(RUSAGE_SELF, &usage);
    long peak_rss_kb = rss_status == 0 ? usage.ru_maxrss : -1;
#if defined(__APPLE__)
    if (rss_status == 0) peak_rss_kb /= 1024; // Darwin reports bytes; Linux KiB.
#endif
    std::printf("CR421 N=%d wall_ms=%.3f per_bar_us=%.3f orders=%d closed=%d peak_rss_kb=%ld\n",
                n, wall_ms, wall_ms * 1000.0 / n, orders, closed, peak_rss_kb);
#if defined(__linux__)
    std::printf("CR421 mech preads=%zu\n", reads);
#endif
    host.print_mechanism();
    std::fflush(stdout);
    require(orders == n, "CR421 pending orders expected " + std::to_string(n)
                        + ", observed " + std::to_string(orders));
    require(closed == 0, "CR421 closed trades expected 0, observed " + std::to_string(closed));
    // The first request is born at the first close; each subsequent open
    // admits exactly the previous close's one new request to the dormant book.
    const auto scanned = host.scanned_rows();
    require(scanned == static_cast<std::uint64_t>(n - 1),
            "CR421 scanned_rows expected N-1, observed " + std::to_string(scanned));
    RunWork work;
    work.wall_ms = wall_ms;
    work.cohort_rows = host.cohort_rows();
#if defined(__linux__)
    work.preads = reads;
#endif
    return work;
}

std::vector<int> parse_grid(const char* value) {
    std::vector<int> grid;
    const char* cursor = value;
    for (;;) {
        char* end = nullptr;
        errno = 0;
        const long count = std::strtol(cursor, &end, 10);
        if (errno != 0 || end == cursor || count <= 0
            || count > std::numeric_limits<int>::max()
            || (*end != ',' && *end != '\0') || grid.size() == 4)
            throw std::runtime_error("CR421 --n requires three or four positive int counts");
        grid.push_back(static_cast<int>(count));
        if (*end == '\0') break;
        cursor = end + 1;
    }
    require(grid.size() >= 3, "CR421 --n requires three or four positive int counts");
    return grid;
}

void require_linear_work(const std::vector<int>& grid,
                         const std::vector<RunWork>& work) {
    // Hand operation budget: Accepted-only bars cannot release an origin or
    // retire a retained prefix. Appending its origin costs one keyed visit;
    // a full cohort walk costs two iterator operations per row (read + next).
    // Eight operations per cohort allow the append plus three full walks
    // and one extra visit, without fitting a bound to observed wall times.
    // A per-bar roster walk violates this even below the spill knee.
    for (std::size_t i = 0; i < grid.size(); ++i)
        require(work[i].cohort_rows <= 8ULL * static_cast<std::uint64_t>(grid[i]),
                "CR421 cohort_iterated_rows exceeds 8*N at N=" + std::to_string(grid[i])
                + ": observed " + std::to_string(work[i].cohort_rows));
    // The default 3:1 population increase permits 4:1 cohort work (4/3
    // slack). Cross multiplication also admits the legitimate all-zero case.
    for (std::size_t i = 1; i < grid.size(); ++i)
        require(static_cast<long double>(work[i].cohort_rows) * grid[0]
                    <= static_cast<long double>(work[0].cohort_rows) * grid[i] * 4.0L / 3.0L,
                "CR421 cohort_iterated_rows growth exceeds linear budget at N="
                + std::to_string(grid[i]));
#if defined(__linux__)
    // One spill per new request and a terminal read of each spill are linear.
    // Allow twice population growth for tree depth / spill-knee overhead:
    // at 7500 / 2500 this is the required maximum ratio of six.
    for (std::size_t i = 1; i < grid.size(); ++i)
        require(static_cast<long double>(work[i].preads) * grid[0]
                    <= static_cast<long double>(work[0].preads) * grid[i] * 2.0L,
                "CR421 preads growth exceeds linear spill budget at N="
                + std::to_string(grid[i]));
#endif
}

double parse_cap(const char* value) {
    char* end = nullptr;
    errno = 0;
    const double seconds = std::strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0'
        || !std::isfinite(seconds) || seconds <= 0.0)
        throw std::runtime_error("CR421 --cap-seconds requires a finite positive number");
    return seconds;
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::vector<int> grid{2500, 5000, 7500, 10000};
        double cap_seconds = 600.0;
        for (int i = 1; i < argc; ++i) {
            if (std::strncmp(argv[i], "--n=", 4) == 0)
                grid = parse_grid(argv[i] + 4);
            else if (std::strncmp(argv[i], "--cap-seconds=", 14) == 0)
                cap_seconds = parse_cap(argv[i] + 14);
            else
                throw std::runtime_error("CR421 usage: [--n=a,b,c[,d]] [--cap-seconds=s]");
        }
        std::vector<RunWork> work(grid.size());
        std::vector<double> walls(grid.size(), kNaN);
        bool capped = false;
        for (std::size_t i = 0; i < grid.size(); ++i) {
            work[i] = run(grid[i]);
            walls[i] = work[i].wall_ms;
            // A completed-leg cap, not an interrupt or a successful stub run.
            // The lab supervisor owns the outer timeout for an individual leg.
            if (walls[i] / 1000.0 > cap_seconds) {
                std::printf("CR421 capped N=%d\n", grid[i]);
                capped = true;
                break;
            }
        }
        const double linear_a = static_cast<double>(grid[1]) / grid[0];
        const double linear_b = static_cast<double>(grid[2]) / grid[1];
        // Unrun legs retain NaN: a capped grid never fabricates a measurement.
        std::printf("CR421 ratios t(%d)/t(%d)=%.3f t(%d)/t(%d)=%.3f "
                    "quadratic_would_be=%.2f,%.2f linear_would_be=%.2f,%.2f\n",
                    grid[1], grid[0], walls[1] / walls[0],
                    grid[2], grid[1], walls[2] / walls[1],
                    linear_a * linear_a, linear_b * linear_b, linear_a, linear_b);
        std::fflush(stdout);
        require(!capped, "CR421 scaling grid incomplete after completed-leg cap");
        require_linear_work(grid, work);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CR421 FAIL: %s\n", error.what());
        return 1;
    }
}
