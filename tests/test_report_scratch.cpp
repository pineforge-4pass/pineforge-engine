// CR-150 L6b-4. The oracle is an independently retained array, never bytes
// copied from the scratch file under test. b3c39238e has neither the internal
// scratch header nor its definitions: fail-before is a missing-header error.
// The common brief reserves the actual fail-before/Release/Debug runs for lab.
#include "global_allocation_replacement.hpp"

#include "../src/report_scratch.hpp"
#include "scratch_descriptor_fixture.hpp"

#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/resource.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace rs = pineforge::report_sink;
namespace metrics = pineforge::metrics;

static_assert(!std::is_copy_constructible<rs::ScratchFile>::value, "file has one owner");
static_assert(!std::is_copy_constructible<rs::ScratchCurveWriter>::value, "curve has one owner");
static_assert(!std::is_copy_constructible<rs::ScratchCurveWindow>::value, "cache has one owner");

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct ScratchDirectory {
    std::string path = "/tmp/pineforge-curve-test-XXXXXX";
    ScratchDirectory() {
        require(::mkdtemp(path.data()) != nullptr, "create test directory");
    }
    ~ScratchDirectory() { ::rmdir(path.c_str()); }

    bool empty() const {
        DIR* directory = ::opendir(path.c_str());
        require(directory != nullptr, "open test directory");
        bool result = true;
        while (const auto* entry = ::readdir(directory)) {
            if (std::strcmp(entry->d_name, ".") != 0
                && std::strcmp(entry->d_name, "..") != 0) result = false;
        }
        ::closedir(directory);
        return result;
    }
};

struct TmpdirGuard {
    bool existed = std::getenv("TMPDIR") != nullptr;
    std::string previous = existed ? std::getenv("TMPDIR") : "";
    explicit TmpdirGuard(const std::string& directory) {
        require(::setenv("TMPDIR", directory.c_str(), 1) == 0, "set test TMPDIR");
    }
    ~TmpdirGuard() {
        if (existed) ::setenv("TMPDIR", previous.c_str(), 1);
        else ::unsetenv("TMPDIR");
    }
};

struct RefuseAllocations {
    RefuseAllocations() { global_allocation::refusing = true; }
    ~RefuseAllocations() { global_allocation::refusing = false; }
};

struct FileSizeLimit {
    struct rlimit previous{};
    using Handler = void (*)(int);
    Handler handler = SIG_DFL;

    explicit FileSizeLimit(rlim_t bytes) {
        require(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "get file-size limit");
        require(previous.rlim_max == RLIM_INFINITY || previous.rlim_max >= bytes,
                "lab hard limit admits test file size");
        handler = std::signal(SIGXFSZ, SIG_IGN);
        require(handler != SIG_ERR, "ignore test file-size signal");
        struct rlimit limit = previous;
        limit.rlim_cur = bytes;
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            std::signal(SIGXFSZ, handler);
            throw std::runtime_error("set file-size limit");
        }
    }
    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &previous);
        std::signal(SIGXFSZ, handler);
    }
};

template <class Operation>
void expect_io(Operation operation, int error_number) {
    bool caught = false;
    try {
        operation();
    } catch (const std::system_error& error) {
        require(error.code().category() == std::generic_category(), "generic errno category");
        require(error.code().value() == error_number, "exact disk errno preserved");
        require(pineforge::classify_run_failure(error).code
                    == pineforge::RunFailureCode::report_sink_failed,
                "scratch disk error carries report_sink_failed");
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.args && *failure.args == "{\"reason\":\"scratch_io\"}",
                "file-mode scratch failures carry only their reason");
        caught = true;
    }
    require(caught, "expected scratch I/O exception");
}

template <class Operation>
void expect_invariant(Operation operation) {
    bool caught = false;
    try {
        operation();
    } catch (const std::logic_error& error) {
        require(pineforge::classify_run_failure(error).code
                    == pineforge::RunFailureCode::engine_invariant,
                "bad scratch range is an engine invariant");
        caught = true;
    }
    require(caught, "expected scratch range exception");
}

bool same_point(const pf_equity_point_t& actual, const pf_equity_point_t& expected) {
    return std::memcmp(&actual, &expected, sizeof(actual)) == 0;
}

std::vector<pf_equity_point_t> random_points(std::size_t count, std::mt19937_64& random) {
    std::vector<pf_equity_point_t> points(count);
    for (std::size_t i = 0; i < count; ++i) {
        points[i].time_ms = static_cast<std::int64_t>(random() & UINT64_C(0x00ffffffffffffff));
        const std::uint64_t equity = random();
        const std::uint64_t open_profit = random();
        std::memcpy(&points[i].equity, &equity, sizeof(equity));
        std::memcpy(&points[i].open_profit, &open_profit, sizeof(open_profit));
    }
    return points;
}

void compare_walks(const metrics::ScratchCurveRegion& region,
                   const std::vector<pf_equity_point_t>& points, std::mt19937_64& random) {
    const metrics::ArrayCurve array(points.data(), static_cast<std::int64_t>(points.size()));
    const rs::ScratchCurveWindow curve(region);
    rs::ScratchCurveWindow independent(region);
    require(curve.size() == array.size(), "scratch and array counts agree");
    if (points.empty()) {
        expect_invariant([&] { (void)curve[0]; });
        return;
    }
    // The metric walk's first/last probes followed by restarted forward
    // passes, reverse traversal and arbitrary jumps across refill boundaries.
    const pf_equity_point_t saved = curve[0];
    require(same_point(curve[curve.size() - 1], array[array.size() - 1]), "last probe matches array");
    for (int pass = 0; pass < 3; ++pass) {
        for (std::int64_t i = 0; i < array.size(); ++i)
            require(same_point(curve[i], array[i]), "forward read matches array bits");
    }
    for (std::int64_t i = array.size(); i > 0; --i)
        require(same_point(curve[i - 1], array[i - 1]), "reverse read matches array bits");
    for (int sample = 0; sample < 128; ++sample) {
        const auto index = static_cast<std::int64_t>(
            random() % static_cast<std::uint64_t>(array.size()));
        require(same_point(curve[index], array[index]), "random window matches array bits");
        const auto other = array.size() - 1 - index;
        require(same_point(independent[other], array[other]), "independent reader matches array");
    }
    require(same_point(saved, array[0]), "returned value survives refills");
    expect_invariant([&] { (void)curve[-1]; });
    expect_invariant([&] { (void)curve[curve.size()]; });
}

void curve_writer_matches_array() {
    ScratchDirectory directory;
    std::mt19937_64 random(UINT64_C(0x17ad4fa3026b0004));
    const std::size_t window = rs::kScratchWindowPoints;
    const std::size_t boundaries[] = {0, 1, 2, window - 1, window, window + 1,
                                      2 * window - 1, 2 * window, 2 * window + 1,
                                      5 * window + 17};
    for (int test = 0; test < 26; ++test) {
        const std::size_t count = test < 10 ? boundaries[test]
            : static_cast<std::size_t>(random() % (6 * window + 1));
        const auto points = random_points(count, random);
        rs::ScratchCurveWriter writer(directory.path);
        for (const auto& point : points) writer.append(point);
        require(writer.size() == static_cast<std::int64_t>(points.size()), "writer accepts every point");
        const auto& const_writer = writer;
        const auto region = const_writer.region();
        require(directory.empty(), "curve file is unlinked while owned");
        const int descriptor = scratch_fixture::descriptor(region);
        struct stat status{};
        require(::fstat(descriptor, &status) == 0, "stat curve file");
        require(status.st_size == static_cast<off_t>(points.size() * sizeof(pf_equity_point_t)),
                "scratch is exactly 24 bytes per point");
        require(::lseek(descriptor, 11, SEEK_SET) == 11, "set shared file cursor");
        compare_walks(region, points, random);
        require(::lseek(descriptor, 0, SEEK_CUR) == 11, "pread preserves file cursor");
        const auto again = writer.region();
        require(again.scratch == region.scratch && again.point_count == region.point_count,
                "repeated region does not duplicate or consume points");
    }
}

void special_double_bits_survive() {
    ScratchDirectory directory;
    const std::uint64_t bits[] = {
        UINT64_C(0), UINT64_C(0x8000000000000000), UINT64_C(1),
        UINT64_C(0x8000000000000001), UINT64_C(0x7ff0000000000000),
        UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000000000042),
        UINT64_C(0xfff8000000000069), UINT64_C(0x7ff0000000000001),
        UINT64_C(0x7fefffffffffffff), UINT64_C(0x0010000000000000),
    };
    std::vector<pf_equity_point_t> points;
    rs::ScratchCurveWriter writer(directory.path);
    for (std::size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
        pf_equity_point_t point{};
        point.time_ms = i % 2 == 0 ? std::numeric_limits<std::int64_t>::min()
                                  : std::numeric_limits<std::int64_t>::max();
        std::memcpy(&point.equity, &bits[i], sizeof(double));
        std::memcpy(&point.open_profit, &bits[sizeof(bits) / sizeof(bits[0]) - i - 1], sizeof(double));
        points.push_back(point);
        writer.append(point);
    }
    std::mt19937_64 random(7);
    compare_walks(writer.region(), points, random);
}

void arbitrary_regions_and_raw_file_lifetime() {
    ScratchDirectory directory;
    TmpdirGuard tmpdir(directory.path + "/TMPDIR-must-not-be-used");
    std::mt19937_64 random(9);
    const auto points = random_points(2 * rs::kScratchWindowPoints + 1, random);
    const std::int64_t offsets[] = {0, 1, 7, 23, 24, 65535};
    for (const auto offset : offsets) {
        int descriptor = -1;
        {
            rs::ScratchFile file(directory.path);
            descriptor = scratch_fixture::descriptor(file.curve_region(0, 0));
            struct stat status{};
            require(::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode)
                        && status.st_nlink == 0 && (status.st_mode & 0777) == 0600,
                    "scratch file is private and anonymous");
            require((::fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0, "scratch closes on exec");
            require(directory.empty(), "raw scratch file has no directory entry");
            file.write(offset, points.data(), points.size() * sizeof(points[0]));
            file.read(0, nullptr, 0);
            const auto region = file.curve_region(offset, static_cast<std::int64_t>(points.size()));
            compare_walks(region, points, random);
            std::array<pf_equity_point_t, 3> block{};
            rs::read_scratch_points(region, static_cast<std::int64_t>(rs::kScratchWindowPoints) - 1,
                                     block.data(), static_cast<std::int64_t>(block.size()));
            for (std::size_t i = 0; i < block.size(); ++i)
                require(same_point(block[i], points[rs::kScratchWindowPoints - 1 + i]),
                        "exact block read crosses the window boundary");
            file.read(offset + static_cast<std::int64_t>(
                          (rs::kScratchWindowPoints - 1) * sizeof(points[0])),
                       block.data(), sizeof(block));
            for (std::size_t i = 0; i < block.size(); ++i)
                require(same_point(block[i], points[rs::kScratchWindowPoints - 1 + i]),
                        "raw position-tail-style read preserves the same binary bytes");
            rs::read_scratch_points(region, region.point_count, nullptr, 0);
        }
        errno = 0;
        require(::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF, "owner destruction closes file");
    }
    expect_io([&] { rs::ScratchFile missing(directory.path + "/missing"); }, ENOENT);
    std::string regular_path = directory.path + "/regular";
    const int regular = ::open(regular_path.c_str(), O_CREAT | O_RDWR, 0600);
    require(regular >= 0, "create directory-refusal fixture");
    ::close(regular);
    expect_io([&] { rs::ScratchFile not_directory(regular_path + "/child"); }, ENOTDIR);
    require(::unlink(regular_path.c_str()) == 0, "remove directory-refusal fixture");
    expect_invariant([&] { rs::ScratchFile embedded_nul(std::string("bad\0directory", 13)); });
    // Empty is the facade's default directory, needed by a borrowed-fd sink.
    expect_io([&] { rs::ScratchFile default_directory(""); }, ENOENT);
}

void replace_clear_and_snapshot_counts() {
    ScratchDirectory directory;
    rs::ScratchCurveWriter writer(directory.path);
    expect_invariant([&] { writer.replace_last({1, 2.0, 3.0}); });
    writer.append({1, 100.0, -0.0});
    writer.replace_last({2, 101.5, 1.5});
    auto region = writer.region();
    require(same_point(rs::ScratchCurveWindow(region)[0], {2, 101.5, 1.5}),
            "replace pending last point");
    writer.replace_last({3, 98.0, -2.0});
    region = writer.region();
    require(same_point(rs::ScratchCurveWindow(region)[0], {3, 98.0, -2.0}),
            "replace already-flushed last point");
    const auto prefix = region;
    writer.append({4, 102.0, 0.0});
    const auto extended = writer.region();
    require(prefix.point_count == 1 && extended.point_count == 2,
            "borrowed region keeps its snapshot count");
    require(same_point(rs::ScratchCurveWindow(prefix)[0], {3, 98.0, -2.0}),
            "append preserves a borrowed prefix");
    const int descriptor = scratch_fixture::descriptor(extended);
    writer.clear();
    require(writer.size() == 0 && writer.region().point_count == 0, "clear resets count");
    errno = 0;
    require(::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF,
            "clear releases the old backing instead of shrinking it");
    writer.append({9, 200.0, 0.0});
    require(same_point(rs::ScratchCurveWindow(writer.region())[0], {9, 200.0, 0.0}),
            "curve can be reused after clear");
}

void malformed_regions_and_short_reads() {
    ScratchDirectory directory;
    rs::ScratchFile file(directory.path);
    const pf_equity_point_t point{7, 100.0, 0.0};
    const metrics::ScratchCurveRegion empty{-1, 0, 0};
    require(rs::ScratchCurveWindow(empty).size() == 0, "empty region needs no live descriptor");
    rs::read_scratch_points(empty, 0, nullptr, 0);
    file.write(0, &point, sizeof(point));
    const int descriptor = scratch_fixture::descriptor(file.curve_region(0, 1));
    const auto limit = static_cast<std::int64_t>(std::numeric_limits<off_t>::max());
    const metrics::ScratchCurveRegion malformed[] = {
        {-1, 0, 1}, {descriptor, -1, 1}, {descriptor, 0, -1},
        {descriptor, limit, 1},
        {descriptor, 0, limit / static_cast<std::int64_t>(sizeof(point)) + 1},
    };
    for (const auto& region : malformed)
        expect_invariant([&] { rs::ScratchCurveWindow rejected(region); });
    const auto valid = file.curve_region(0, 1);
    expect_invariant([&] { rs::read_scratch_points(valid, -1, nullptr, 0); });
    expect_invariant([&] { rs::read_scratch_points(valid, 0, nullptr, -1); });
    expect_invariant([&] { rs::read_scratch_points(valid, 1, nullptr, 1); });
    expect_invariant([&] { rs::read_scratch_points(valid, 0, nullptr, 1); });
    expect_invariant([&] { file.write(-1, &point, sizeof(point)); });
    expect_invariant([&] { file.write(limit, &point, sizeof(point)); });
    expect_invariant([&] { file.ensure_size(-1); });
    require(::ftruncate(descriptor, sizeof(point) - 1) == 0, "inject a short backing");
    expect_io([&] { (void)rs::ScratchCurveWindow(valid)[0]; }, EIO);

    const int borrowed = ::dup(descriptor);
    require(borrowed >= 0, "duplicate descriptor for closed-reader test");
    require(::close(borrowed) == 0, "close borrowed descriptor");
    const metrics::ScratchCurveRegion closed{borrowed, 0, 1};
    expect_io([&] { (void)rs::ScratchCurveWindow(closed)[0]; }, EBADF);
    int pipe_descriptors[2]{};
    require(::pipe(pipe_descriptors) == 0, "create nonseekable reader fixture");
    const metrics::ScratchCurveRegion unseekable{pipe_descriptors[0], 0, 1};
    expect_io([&] { (void)rs::ScratchCurveWindow(unseekable)[0]; }, ESPIPE);
    ::close(pipe_descriptors[0]);
    ::close(pipe_descriptors[1]);
}

void failed_refill_invalidates_old_cache() {
    ScratchDirectory directory;
    rs::ScratchFile file(directory.path);
    std::mt19937_64 random(11);
    const auto points = random_points(2 * rs::kScratchWindowPoints, random);
    const auto length = points.size() * sizeof(points[0]);
    file.write(0, points.data(), length);
    const auto region = file.curve_region(0, static_cast<std::int64_t>(points.size()));
    const int descriptor = scratch_fixture::descriptor(region);
    const rs::ScratchCurveWindow curve(region);
    require(same_point(curve[0], points[0]), "prime first cache window");
    require(::ftruncate(descriptor,
                        static_cast<off_t>(rs::kScratchWindowPoints * sizeof(points[0]) + 12)) == 0,
            "inject a partial next window");
    expect_io([&] { (void)curve[static_cast<std::int64_t>(rs::kScratchWindowPoints)]; }, EIO);
    file.write(0, points.data(), length);
    require(same_point(curve[0], points[0]), "failed refill cannot expose partially overwritten old cache");
    require(same_point(curve[curve.size() - 1], points.back()), "reader retries after backing file repaired");
}

void partial_flush_is_retryable() {
    ScratchDirectory directory;
    rs::ScratchCurveWriter writer(directory.path);
    std::mt19937_64 random(13);
    const auto points = random_points(rs::kScratchWindowPoints + 1, random);
    for (std::size_t i = 0; i < rs::kScratchWindowPoints; ++i) writer.append(points[i]);
    {
        // Not a multiple of 24: complete-extent growth refuses before publish.
        FileSizeLimit limit(97);
        expect_io([&] { writer.append(points.back()); }, EFBIG);
    }
    require(writer.size() == static_cast<std::int64_t>(rs::kScratchWindowPoints),
            "failed previous-batch flush does not accept the next point");
    writer.append(points.back());
    compare_walks(writer.region(), points, random);
    const auto replacement = points[3];
    writer.replace_last(replacement);
    {
        FileSizeLimit limit(97);
        expect_io([&] { writer.flush(); }, EFBIG);
    }
    const auto fixed = writer.region();
    require(fixed.point_count == static_cast<std::int64_t>(points.size()), "failed replacement keeps count");
    require(same_point(rs::ScratchCurveWindow(fixed)[fixed.point_count - 1], replacement),
            "failed replacement flush retains all replacement bits for retry");
}

void storage_stays_bounded_after_construction() {
    ScratchDirectory directory;
    rs::ScratchCurveWriter writer(directory.path);
    // Construct before refusing allocations: State owns the one fixed batch.
    const std::int64_t count = static_cast<std::int64_t>(rs::kScratchWindowPoints) * 24 + 19;
    const auto allocation_count = global_allocation::allocations;
    {
        RefuseAllocations guard;
        for (std::int64_t i = 0; i < count; ++i)
            writer.append({i, static_cast<double>(i) + 0.25, -static_cast<double>(i)});
        writer.flush();
    }
    require(global_allocation::allocations == allocation_count, "append/flush allocate no growing storage");
    const auto region = writer.region();
    const rs::ScratchCurveWindow curve(region);
    const auto reader_allocations = global_allocation::allocations;
    {
        RefuseAllocations guard;
        for (std::int64_t i = count; i > 0; --i) {
            const auto point = curve[i - 1];
            require(same_point(point, {i - 1, static_cast<double>(i - 1) + 0.25,
                                      -static_cast<double>(i - 1)}),
                    "allocation-free refill matches independent formula");
        }
    }
    require(global_allocation::allocations == reader_allocations, "pread refills allocate no growing storage");
}

void range_end_fold_matches_array() {
    ScratchDirectory directory;
    std::mt19937_64 random(17);
    for (int test = 0; test < 14; ++test) {
        const auto count = static_cast<std::size_t>(random() % (3 * rs::kScratchWindowPoints));
        rs::ScratchCurveWriter prefix(directory.path);
        std::vector<pf_equity_point_t> points;
        double equity = 10000.0;
        for (std::size_t i = 0; i < count; ++i) {
            equity += static_cast<double>(static_cast<std::int64_t>(random() % 4001) - 2000) / 16.0;
            points.push_back({static_cast<std::int64_t>(i), equity, 0.0});
            prefix.append(points.back());
        }
        for (int replacement = 0; replacement < 3; ++replacement) {
            const pf_equity_point_t held{
                static_cast<std::int64_t>(count), equity + (replacement - 1) * 400.0, -0.0};
            double expected[] = {10000.0, 10000.0, 0.0, 0.0};
            // Independent in-memory reference, including the original seed.
            const auto fold = [&](double value) {
                if (value > expected[0]) { expected[0] = value; expected[1] = value; }
                if (value < expected[1]) expected[1] = value;
                expected[2] = std::max(expected[2], expected[0] - value);
                expected[3] = std::max(expected[3], value - expected[1]);
            };
            for (const auto& point : points) fold(point.equity);
            const double before_held[] = {expected[0], expected[1], expected[2], expected[3]};
            rs::check_scratch_equity_extremes(prefix.region(), nullptr, 10000.0, before_held);
            fold(held.equity);
            rs::check_scratch_equity_extremes(prefix.region(), &held, 10000.0, expected);
            require(prefix.size() == static_cast<std::int64_t>(count),
                    "Debug reference does not append the editable held point");
            double wrong[] = {expected[0], expected[1], expected[2] + 1.0, expected[3]};
            expect_invariant([&] {
                rs::check_scratch_equity_extremes(prefix.region(), &held, 10000.0, wrong);
            });
        }
    }
}

void scratch_metrics_match_array() {
    ScratchDirectory directory;
    std::mt19937_64 random(23);
    const std::size_t counts[] = {
        0, 1, 2, 3, rs::kScratchWindowPoints - 1, rs::kScratchWindowPoints,
        rs::kScratchWindowPoints + 1, 2 * rs::kScratchWindowPoints + 13,
    };
    for (const auto count : counts) {
        std::vector<pf_equity_point_t> points;
        rs::ScratchCurveWriter writer(directory.path);
        double equity = 10000.0;
        for (std::size_t i = 0; i < count; ++i) {
            equity += static_cast<double>(static_cast<std::int64_t>(random() % 1001) - 500) / 32.0;
            points.push_back({1704067199000LL + static_cast<std::int64_t>(i) * 86400000LL,
                              equity, static_cast<double>(i % 7) - 3.0});
            writer.append(points.back());
        }
        // Model final presentation on a copy; neither path recomputes or
        // rounds the curve's last point differently.
        if (!points.empty()) {
            points.back().time_ms += 3600000;
            points.back().equity -= 0.75;
            points.back().open_profit = -0.0;
            writer.replace_last(points.back());
        }
        const metrics::ArrayCurve array(points.data(), static_cast<std::int64_t>(points.size()));
        const rs::ScratchCurveWindow scratch(writer.region());
        for (const std::string zone : {"UTC", "America/New_York"}) {
            const auto n = array.size();
            const double profit = points.empty() ? 0.0 : points.back().equity - 10000.0;
            const auto expected = metrics::compute_equity_stats<metrics::ArrayCurve>(
                array, n, 10000.0, zone, 100.0, 97.0, n / 3, profit);
            const auto actual = metrics::compute_equity_stats<rs::ScratchCurveWindow>(
                scratch, n, 10000.0, zone, 100.0, 97.0, n / 3, profit);
            require(std::memcmp(&actual, &expected, sizeof(actual)) == 0,
                    "all equity metrics match array with ordinary denominator");
            const auto observations = std::max<std::int64_t>(0, n - 1);
            const auto selected_expected = metrics::compute_equity_stats<metrics::ArrayCurve>(
                array, n, 10000.0, zone, 100.0, 97.0, n / 3, profit, observations);
            const auto selected_actual = metrics::compute_equity_stats<rs::ScratchCurveWindow>(
                scratch, n, 10000.0, zone, 100.0, 97.0, n / 3, profit, observations);
            require(std::memcmp(&selected_actual, &selected_expected, sizeof(selected_actual)) == 0,
                    "all equity metrics match array with selected anchor denominator");
        }
    }
}

} // namespace

int main() {
    try {
        curve_writer_matches_array();
        special_double_bits_survive();
        arbitrary_regions_and_raw_file_lifetime();
        replace_clear_and_snapshot_counts();
        malformed_regions_and_short_reads();
        failed_refill_invalidates_old_cache();
        partial_flush_is_retryable();
        storage_stays_bounded_after_construction();
        range_end_fold_matches_array();
        scratch_metrics_match_array();
        std::puts("report scratch: array equality, bounded windows, I/O mapping and tail fold passed");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::refusing = false;
        std::fprintf(stderr, "report scratch: %s\n", error.what());
        return 1;
    }
}
