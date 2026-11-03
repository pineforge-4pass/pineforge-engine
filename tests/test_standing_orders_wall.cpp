// Measure how work scales while unfilled standing orders stay pending.
// Wall times and ratios are observations, never pass/fail thresholds.
// No product implementation unit or mutation seam is included.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/native_execution_consumer.hpp"

#include <sys/resource.h>

#include <algorithm>
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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace dormant_io {
// Counter-only part of test_native_dormant_book_cost's dormant_io observer.
// Every call reaches the real syscall; no result/errno/data is synthesized.
inline std::size_t preads = 0;
inline std::size_t small_preads = 0;
inline std::size_t read_bytes = 0;
struct FileReads {
    dev_t device{};
    ino_t inode{};
    std::array<char, 512> name{};
    std::size_t reads = 0, small = 0, bytes = 0, requested = 0;
    std::uint64_t order_id = 0;
    std::uint64_t max_next_handle = 0, max_handle_height = 0, max_index_height = 0;
};
inline std::array<FileReads, 256> files{};
inline std::size_t file_count = 0;
inline bool incomplete = false;
inline bool measuring = false;

// Follow the existing id-cache observer's device/inode identity, including
// files closed and replaced during a run. Names are diagnostics only; neither
// the product nor the work gate selects behavior from a scratch-file name.
FileReads* identify(int fd) noexcept {
    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        incomplete = true;
        return nullptr; // Still forward the original pread, including its errno.
    }
    for (std::size_t i = 0; i < file_count; ++i)
        if (files[i].device == info.st_dev && files[i].inode == info.st_ino) return &files[i];
    if (file_count == files.size()) {
        incomplete = true;
        return nullptr;
    }
    auto& file = files[file_count++];
    file = {};
    file.device = info.st_dev;
    file.inode = info.st_ino;
    char link[64]{}, path[512]{};
    std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    const auto length = ::readlink(link, path, sizeof path - 1);
    const char* name = length > 0 ? std::strrchr(path, '/') : nullptr;
    std::snprintf(file.name.data(), file.name.size(), "%s",
                  name ? name + 1 : length > 0 ? path : "unresolved");
    return &file;
}

std::uint64_t word(const unsigned char* data) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t{data[i]} << (8 * i);
    return value;
}

// Observe the real published superblock, without issuing a diagnostic read
// or changing the syscall's bytes/result. The persisted comparator identity
// ties a device/inode to its creator's ScratchKeyOrder, even when several
// owners use the same scratch-file prefix. It never selects product behavior.
void published_store(int fd, const void* data, std::size_t bytes, off_t offset) noexcept {
    if (!measuring || bytes < 13 * sizeof(std::uint64_t)) return;
    const auto* raw = static_cast<const unsigned char*>(data);
    if (word(raw) != UINT64_C(0x3142535243534650) || word(raw + 8) != 1) return;
    const auto page_size = word(raw + 16);
    if (page_size != bytes || (offset != 0 && static_cast<std::uint64_t>(offset) != page_size))
        return;
    auto* file = identify(fd);
    if (!file) return;
    const auto order = word(raw + 24);
    if (file->order_id != 0 && file->order_id != order) incomplete = true;
    file->order_id = order;
    file->max_next_handle = std::max(file->max_next_handle, word(raw + 40));
    file->max_handle_height = std::max(file->max_handle_height, word(raw + 80));
    file->max_index_height = std::max(file->max_index_height, word(raw + 96));
}
}

extern "C" ssize_t pread(int fd, void* data, std::size_t bytes, off_t offset) {
    const int saved = errno;
    auto* file = dormant_io::measuring ? dormant_io::identify(fd) : nullptr;
    errno = saved;
    ++dormant_io::preads;
    if (bytes >= 8 && bytes <= 64) ++dormant_io::small_preads;
    const auto result = static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, bytes, offset));
    const int error = errno;
    if (result > 0) dormant_io::read_bytes += static_cast<std::size_t>(result);
    if (file) {
        ++file->reads;
        file->small += bytes >= 8 && bytes <= 64;
        file->requested += bytes;
        if (result > 0) file->bytes += static_cast<std::size_t>(result);
    }
    errno = error;
    return result;
}

extern "C" ssize_t pwrite(int fd, const void* data, std::size_t bytes, off_t offset) {
    const auto result = static_cast<ssize_t>(::syscall(SYS_pwrite64, fd, data, bytes, offset));
    const int error = errno;
    if (result >= 0 && static_cast<std::size_t>(result) == bytes)
        dormant_io::published_store(fd, data, bytes, offset);
    errno = error;
    return result;
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
    std::size_t read_bytes = 0;
};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Print dormant-book diagnostics only when the consumer exposes them.
// Accessor substitution keeps older consumers usable without fabricated data.
template<class Consumer>
auto print_dormant(const Consumer& consumer, int)
    -> decltype(consumer.scanned_rows(), consumer.fast_points(),
                consumer.eager_points(), consumer.projected_reads(), void()) {
    std::printf("standing-orders mech scanned_rows=%llu fast_points=%llu eager_points=%llu projected_reads=%llu\n",
                static_cast<unsigned long long>(consumer.scanned_rows()),
                static_cast<unsigned long long>(consumer.fast_points()),
                static_cast<unsigned long long>(consumer.eager_points()),
                static_cast<unsigned long long>(consumer.projected_reads()));
}
template<class Consumer>
void print_dormant(const Consumer&, long) {
    std::printf("standing-orders mech dormant_counters=unavailable\n");
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
        std::printf("standing-orders mech cohort_spill_rows=%llu cohort_capacity_rows=%llu "
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
        std::printf("standing-orders mech book_lookup_rows=%llu book_lookup_steps=%llu\n",
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
    const auto small_before = dormant_io::small_preads;
    const auto bytes_before = dormant_io::read_bytes;
    dormant_io::file_count = 0;
    dormant_io::incomplete = false;
    dormant_io::measuring = true;
#endif
    const auto begin = std::chrono::steady_clock::now();
    // Same public PineStrategyHost entry as generated Pine: prepare_native_begin
    // provisions the adapter, native consumer, run arena and scratch tables.
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    const auto end = std::chrono::steady_clock::now();
#if defined(__linux__)
    dormant_io::measuring = false;
    const auto reads = dormant_io::preads - reads_before;
    const auto small_reads = dormant_io::small_preads - small_before;
    const auto read_bytes = dormant_io::read_bytes - bytes_before;
#endif
    const double wall_ms = std::chrono::duration<double, std::milli>(end - begin).count();
    const auto lifecycle = host.native_state().kind;
    require(lifecycle == NativeLifecycleKind::Completed,
            "standing-orders run did not reach Completed: " + host.last_error());

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
    std::printf("standing-orders N=%d wall_ms=%.3f per_bar_us=%.3f orders=%d closed=%d peak_rss_kb=%ld\n",
                n, wall_ms, wall_ms * 1000.0 / n, orders, closed, peak_rss_kb);
#if defined(__linux__)
    std::printf("standing-orders mech preads=%zu preads_per_order=%.3f "
                "small_preads=%zu read_bytes=%zu\n",
                reads, static_cast<double>(reads) / n,
                small_reads, read_bytes);
    std::size_t file_reads = 0, file_bytes = 0, file_small = 0;
    for (std::size_t i = 0; i < dormant_io::file_count; ++i) {
        const auto& file = dormant_io::files[i];
        file_reads += file.reads;
        file_small += file.small;
        file_bytes += file.bytes;
        std::printf("standing-orders file=%zu name=%s preads=%zu small_preads=%zu "
                    "read_bytes=%zu requested=%zu\n", i, file.name.data(),
                    file.reads, file.small, file.bytes, file.requested);
        if (file.order_id != 0)
            std::printf("standing-orders file=%zu order_id=%016llx max_next_handle=%llu "
                        "max_handle_height=%llu max_index_height=%llu\n", i,
                        static_cast<unsigned long long>(file.order_id),
                        static_cast<unsigned long long>(file.max_next_handle),
                        static_cast<unsigned long long>(file.max_handle_height),
                        static_cast<unsigned long long>(file.max_index_height));
    }
    require(!dormant_io::incomplete
            && file_reads == reads && file_small == small_reads && file_bytes == read_bytes,
            "per-file observer accounts for every measured read and byte");
#endif
    host.print_mechanism();
    std::fflush(stdout);
    require(orders == n, "standing-orders pending orders expected " + std::to_string(n)
                        + ", observed " + std::to_string(orders));
    require(closed == 0, "standing-orders closed trades expected 0, observed " + std::to_string(closed));
    // The first request is born at the first close; each subsequent open
    // admits exactly the previous close's one new request to the dormant book.
    const auto scanned = host.scanned_rows();
    require(scanned == static_cast<std::uint64_t>(n - 1),
            "standing-orders scanned_rows expected N-1, observed " + std::to_string(scanned));
    RunWork work;
    work.wall_ms = wall_ms;
    work.cohort_rows = host.cohort_rows();
#if defined(__linux__)
    work.preads = reads;
    work.read_bytes = read_bytes;
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
            || (*end != ',' && *end != '\0') || grid.size() == 5)
            throw std::runtime_error("standing-orders --n requires three to five positive int counts");
        grid.push_back(static_cast<int>(count));
        if (grid.size() > 1)
            require(grid.back() > grid[grid.size() - 2],
                    "standing-orders --n counts must increase");
        if (*end == '\0') break;
        cursor = end + 1;
    }
    require(grid.size() >= 3, "standing-orders --n requires three to five positive int counts");
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
                "standing-orders cohort_iterated_rows exceeds 8*N at N=" + std::to_string(grid[i])
                + ": observed " + std::to_string(work[i].cohort_rows));
    // The default 3:1 population increase permits 4:1 cohort work (4/3
    // slack). Cross multiplication also admits the legitimate all-zero case.
    for (std::size_t i = 1; i < grid.size(); ++i)
        require(static_cast<long double>(work[i].cohort_rows) * grid[0]
                    <= static_cast<long double>(work[0].cohort_rows) * grid[i] * 4.0L / 3.0L,
                "standing-orders cohort_iterated_rows growth exceeds linear budget at N="
                + std::to_string(grid[i]));
#if defined(__linux__)
    // Subtract adjacent runs to remove the fixed resident-book warmup cost.
    // With constant work per appended spilled order, these marginal rates
    // stay flat. Permit 1/8 block/rotation rounding against the FIRST interval,
    // never against a drifting preceding interval. The partial repair's
    // 146.873 -> 175.953 -> 180.837 -> 264.086 reads per added order fails.
    // Count returned bytes too: replacing small reads with growing full-file
    // reads cannot satisfy the contract by hiding the work in larger calls.
    const auto base_rows = grid[1] - grid[0];
    const auto base_reads = static_cast<long double>(work[1].preads) - work[0].preads;
    const auto base_bytes = static_cast<long double>(work[1].read_bytes) - work[0].read_bytes;
    require(base_reads > 0 && base_bytes > 0, "standing-orders marginal observer sees real reads");
    bool flat_reads = true, flat_bytes = true;
    for (std::size_t i = 1; i < grid.size(); ++i) {
        const auto rows = grid[i] - grid[i - 1];
        const auto reads = static_cast<long double>(work[i].preads) - work[i - 1].preads;
        const auto bytes = static_cast<long double>(work[i].read_bytes) - work[i - 1].read_bytes;
        std::printf("standing-orders marginal N=%d..%d preads_per_added_order=%.3Lf "
                    "bytes_per_added_order=%.3Lf\n",
                    grid[i - 1], grid[i], reads / rows, bytes / rows);
        flat_reads = flat_reads && reads >= 0 && reads * base_rows * 8 <= base_reads * rows * 9;
        flat_bytes = flat_bytes && bytes >= 0 && bytes * base_rows * 8 <= base_bytes * rows * 9;
    }
    std::fflush(stdout);
    // Preserve the historical whole-run guard, after printing every marginal
    // row so a failure still leaves the complete native growth diagnostics.
    for (std::size_t i = 1; i < grid.size(); ++i)
        require(static_cast<long double>(work[i].preads) * grid[0]
                    <= static_cast<long double>(work[0].preads) * grid[i] * 2.0L,
                "standing-orders preads growth exceeds linear spill budget at N="
                + std::to_string(grid[i]));
    require(flat_reads, "standing-orders marginal preads per added order grow beyond 9/8");
    require(flat_bytes, "standing-orders marginal bytes per added order grow beyond 9/8");
#endif
}

} // namespace

int main(int argc, char** argv) {
    try {
        // The reference point is the first entry; it sits above the 4,096
        // resident cap so the budget below never mixes the resident regime
        // into the baseline. Keep #621's 20,000-order row and every original
        // pending-order/trade/scanned-row witness in the marginal growth gate.
        std::vector<int> grid{5000, 7500, 10000, 15000, 20000};
        for (int i = 1; i < argc; ++i) {
            if (std::strncmp(argv[i], "--n=", 4) == 0)
                grid = parse_grid(argv[i] + 4);
            else
                throw std::runtime_error("standing-orders usage: [--n=a,b,c[,d[,e]]]");
        }
        std::vector<RunWork> work(grid.size());
        std::vector<double> walls(grid.size(), kNaN);
        for (std::size_t i = 0; i < grid.size(); ++i) {
            work[i] = run(grid[i]);
            walls[i] = work[i].wall_ms;
        }
        const double linear_a = static_cast<double>(grid[1]) / grid[0];
        const double linear_b = static_cast<double>(grid[2]) / grid[1];
        std::printf("standing-orders ratios t(%d)/t(%d)=%.3f t(%d)/t(%d)=%.3f "
                    "quadratic_would_be=%.2f,%.2f linear_would_be=%.2f,%.2f\n",
                    grid[1], grid[0], walls[1] / walls[0],
                    grid[2], grid[1], walls[2] / walls[1],
                    linear_a * linear_a, linear_b * linear_b, linear_a, linear_b);
        std::fflush(stdout);
        require_linear_work(grid, work);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "standing-orders FAIL: %s\n", error.what());
        return 1;
    }
}
