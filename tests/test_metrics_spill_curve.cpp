// CR-150 L7c. Compare ArrayCurve, SpillCurve and the UNMODIFIED frozen
// equity_stats_reference.hpp, including repeated walks and selected counts.
//
// FAIL-BEFORE on b3c39238ef: the frozen P00 SpillCurve declaration has no
// constructor/destructor/index definitions or metric-template instantiations.
// The new reader header/constructor is also absent. A historical-header
// compile/link diagnostic must be captured by CORE-TOP; no local compile was
// permitted by the common lane brief.
#include <pineforge/metrics_stream.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/spill_curve_reader.hpp>

#include "equity_stats_reference.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

using pineforge::metrics::ArrayCurve;
using pineforge::metrics::ScratchCurveReader;
using pineforge::metrics::ScratchCurveRegion;
using pineforge::metrics::SpillCurve;
using pineforge::metrics::compute_equity_stats;

constexpr std::int64_t points_per_window =
    64 * 1024 / static_cast<std::int64_t>(sizeof(pf_equity_point_t));
constexpr double initial_capital = 10000.0;
int failures = 0;
std::int64_t checks = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            if (++failures <= 20)                                               \
                std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                             #condition);                                       \
        }                                                                       \
    } while (0)

bool same_point(const pf_equity_point_t& a, const pf_equity_point_t& b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

void check_stats(const pf_equity_stats_t& actual, const pf_equity_stats_t& expected,
                 const char* source, std::int64_t n, const std::string& zone) {
    const char* field = equity_stats_reference::first_difference(actual, expected);
    CHECK(field == nullptr);
    if (field != nullptr && failures <= 20) {
        std::fprintf(stderr, "  %s: n=%lld zone=%s field=%s\n", source,
                     static_cast<long long>(n), zone.c_str(), field);
    }
}

template <class Exception, class Function>
bool throws_as(Function function) {
    try {
        function();
    } catch (const Exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

// A file-like byte store, deliberately offset from the start of its backing
// allocation. Only the callback can read it; no direct point array is exposed
// to SpillCurve. The vector is TEST backing storage, never production state.
class MemoryScratch {
public:
    explicit MemoryScratch(const std::vector<pf_equity_point_t>& points)
        : point_count_(static_cast<std::int64_t>(points.size())),
          bytes_(prefix_bytes + points.size() * sizeof(pf_equity_point_t) + 19, 0xA5) {
        if (!points.empty()) {
            std::memcpy(bytes_.data() + prefix_bytes, points.data(),
                        points.size() * sizeof(pf_equity_point_t));
        }
    }

    ScratchCurveReader reader() const {
        return {this, point_count_, read};
    }

    mutable std::int64_t reads = 0;
    mutable std::int64_t largest_read = 0;
    mutable std::int64_t last_index = -1;
    mutable std::int64_t last_count = -1;
    mutable bool fail_next = false;

private:
    static constexpr std::size_t prefix_bytes = 13;
    const std::int64_t point_count_;
    std::vector<unsigned char> bytes_;

    static void read(const void* context, std::int64_t index,
                     std::int64_t count, pf_equity_point_t* output) {
        const auto& self = *static_cast<const MemoryScratch*>(context);
        if (index < 0 || count <= 0 || index > self.point_count_ ||
            count > self.point_count_ - index) {
            throw std::out_of_range("test scratch read outside its region");
        }
        if (count > points_per_window) {
            throw std::logic_error("test scratch read exceeds 64 KiB");
        }
        ++self.reads;
        self.largest_read = std::max(self.largest_read, count);
        self.last_index = index;
        self.last_count = count;
        const auto offset =
            prefix_bytes + static_cast<std::size_t>(index) * sizeof(*output);
        if (self.fail_next) {
            self.fail_next = false;
            // Simulate a partial fill that corrupts the OLD cached window,
            // then fails. A retry of an old index must refill it.
            std::memcpy(output, self.bytes_.data() + offset, sizeof(*output));
            throw std::system_error(EIO, std::generic_category(), "test partial read");
        }
        std::memcpy(output, self.bytes_.data() + offset,
                    static_cast<std::size_t>(count) * sizeof(*output));
    }
};

std::vector<pf_equity_point_t> random_curve(std::int64_t n, std::uint64_t seed,
                                          std::int64_t step, bool nonpositive) {
    std::mt19937_64 random(seed);
    std::vector<pf_equity_point_t> points;
    points.reserve(static_cast<std::size_t>(n));
    double equity = initial_capital;
    std::int64_t time = 1546300800000LL; // 2019-01-01 UTC
    for (std::int64_t i = 0; i < n; ++i) {
        equity += static_cast<double>(static_cast<std::int64_t>(random() % 20001) - 10000)
            / 64.0;
        if (nonpositive) {
            if (i % 83 == 19) equity = 0.0;
            else if (i % 83 == 37) equity = -250.0;
            else if (i % 83 == 61) equity = initial_capital;
        }
        const double open =
            static_cast<double>(static_cast<std::int64_t>(random() % 4097) - 2048) / 16.0;
        points.push_back({time, equity, open});
        time += step * static_cast<std::int64_t>(1 + random() % 3);
    }
    return points;
}

void three_way_metrics(const std::vector<pf_equity_point_t>& points,
                       const std::string& zone, std::int64_t walk_count = -1) {
    const auto source_count = static_cast<std::int64_t>(points.size());
    const std::int64_t n = walk_count < 0 ? source_count : walk_count;
    MemoryScratch file(points);
    const SpillCurve spill(file.reader());
    const ArrayCurve array(points.empty() ? nullptr : points.data(), source_count);
    const double last_close =
        n == 0 ? 100.0 : 100.0 + points[static_cast<std::size_t>(n - 1)].equity / 100.0;
    const double net_profit =
        n == 0 ? 0.0 : points[static_cast<std::size_t>(n - 1)].equity - initial_capital;
    const std::int64_t bars_in_market = n / 3;
    const auto reference = equity_stats_reference::compute_equity_stats(
        points.empty() ? nullptr : points.data(), n, initial_capital, zone,
        100.0, last_close, bars_in_market, net_profit);

    const auto from_array = compute_equity_stats<ArrayCurve>(
        array, n, initial_capital, zone, 100.0, last_close, bars_in_market, net_profit);
    const auto from_spill = compute_equity_stats<SpillCurve>(
        spill, n, initial_capital, zone, 100.0, last_close, bars_in_market, net_profit);
    check_stats(from_array, reference, "array/reference", n, zone);
    check_stats(from_spill, reference, "spill/reference", n, zone);
    check_stats(from_spill, from_array, "spill/array", n, zone);

    // Reuse the same source after all five walks ended in its last window.
    // Only exposure changes for the selected-window observation denominator.
    for (const std::int64_t observations : {n, n > 0 ? n - 1 : 0, std::int64_t{0}}) {
        auto selected_reference = reference;
        if (n > 0) {
            selected_reference.time_in_market_pct = observations > 0
                ? static_cast<double>(bars_in_market) / static_cast<double>(observations) * 100.0
                : std::numeric_limits<double>::quiet_NaN();
        }
        const auto selected_array = compute_equity_stats<ArrayCurve>(
            array, n, initial_capital, zone, 100.0, last_close, bars_in_market,
            net_profit, observations);
        const auto selected_spill = compute_equity_stats<SpillCurve>(
            spill, n, initial_capital, zone, 100.0, last_close, bars_in_market,
            net_profit, observations);
        check_stats(selected_array, selected_reference, "selected array/reference", n, zone);
        check_stats(selected_spill, selected_reference, "selected spill/reference", n, zone);
        check_stats(selected_spill, selected_array, "selected spill/array", n, zone);
    }
    CHECK(spill.size() == source_count);
    CHECK(file.largest_read <= points_per_window);
    CHECK(n == 0 ? file.reads == 0 : file.reads > 0);
}

void random_metrics_match_frozen_reference() {
    const std::int64_t counts[] = {
        0, 1, 2, 3, points_per_window - 1, points_per_window, points_per_window + 1,
        2 * points_per_window - 1, 2 * points_per_window, 2 * points_per_window + 1,
        7 * points_per_window + 19,
    };
    for (const auto n : counts) {
        const auto points = random_curve(n, 0x73A120F1ULL + static_cast<std::uint64_t>(n),
                                          86400000LL, false);
        three_way_metrics(points, "UTC");
        three_way_metrics(points, "America/New_York");
        if (n > 2) three_way_metrics(points, "UTC", n - 1); // subrange of a larger source
    }
    const std::string zones[] = {"", "UTC", "Etc/UTC", "America/New_York", "Asia/Taipei"};
    const std::int64_t steps[] = {60000LL, 3600000LL, 86400000LL, 604800000LL};
    std::mt19937_64 random(0x18C7045EULL);
    for (std::size_t trial = 0; trial < 32; ++trial) {
        const auto n = static_cast<std::int64_t>(random() %
            static_cast<std::uint64_t>(4 * points_per_window + 23));
        const auto points = random_curve(n, random(), steps[trial % 4], trial % 2 == 0);
        three_way_metrics(points, zones[trial % 5]);
    }
    // Selected M=0 has one anchor point and zero observations.
    three_way_metrics({{1704067200000LL, initial_capital, 0.0}}, "UTC");
}

void month_boundaries_at_window_edges() {
    struct Boundary { std::int64_t time; const char* zone; };
    const Boundary boundaries[] = {
        {1580515200000LL, "UTC"},              // 2020-02-01
        {1583020800000LL, "UTC"},              // 2020-03-01, leap February
        {1609459200000LL, "UTC"},              // 2021-01-01, year transition
        {1583038800000LL, "America/New_York"}, // 2020-03-01 local midnight
        {1604203200000LL, "America/New_York"}, // 2020-11-01 local midnight
    };
    for (const auto& boundary : boundaries) {
        for (const std::int64_t delta : {-1, 0, 1}) {
            auto points = random_curve(3 * points_per_window + 5, 0xD190F3ULL,
                                        3600000LL, false);
            const std::int64_t at = points_per_window + delta;
            for (std::int64_t i = 0; i < static_cast<std::int64_t>(points.size()); ++i) {
                points[static_cast<std::size_t>(i)].time_ms =
                    boundary.time + (i - at) * 3600000LL;
            }
            points[static_cast<std::size_t>(at - 1)].time_ms = boundary.time - 1;
            points[static_cast<std::size_t>(at + 1)].time_ms = boundary.time + 1;
            three_way_metrics(points, boundary.zone);
        }
    }
}

void cache_edges_and_repeated_walks() {
    const auto points = random_curve(3 * points_per_window + 7, 0x46E5D2ULL, 60000, false);
    MemoryScratch file(points);
    const SpillCurve source(file.reader());
    CHECK(source.size() == static_cast<std::int64_t>(points.size()));
    CHECK(file.reads == 0); // construction and size do no I/O
    const auto first = source[0];
    CHECK(same_point(first, points.front()));
    CHECK(file.reads == 1);
    CHECK(file.last_index == 0 && file.last_count == points_per_window);
    CHECK(same_point(source[points_per_window - 1],
                     points[static_cast<std::size_t>(points_per_window - 1)]));
    CHECK(file.reads == 1);
    CHECK(same_point(source[points_per_window],
                     points[static_cast<std::size_t>(points_per_window)]));
    CHECK(file.reads == 2);
    const auto last_index = source.size() - 1;
    CHECK(same_point(source[last_index], points.back()));
    CHECK(file.last_index == 3 * points_per_window && file.last_count == 7);
    CHECK(same_point(first, points.front())); // retained value survives refills

    for (int pass = 0; pass < 3; ++pass) {
        const auto before = file.reads;
        for (std::int64_t i = 0; i < source.size(); ++i) {
            CHECK(same_point(source[i], points[static_cast<std::size_t>(i)]));
        }
        CHECK(file.reads - before == 4); // one fill per window per forward walk
    }
    for (std::int64_t i = source.size(); i-- > 0;) {
        CHECK(same_point(source[i], points[static_cast<std::size_t>(i)]));
    }
    std::mt19937_64 random(0x4C29ULL);
    for (int trial = 0; trial < 512; ++trial) {
        const auto i = static_cast<std::int64_t>(
            random() % static_cast<std::uint64_t>(source.size()));
        CHECK(same_point(source[i], points[static_cast<std::size_t>(i)]));
    }
    CHECK(file.largest_read == points_per_window);
}

void exact_payloads_and_failed_refills() {
    auto points = random_curve(2 * points_per_window + 1, 0x9D3ULL, 60000, false);
    const std::uint64_t equity_bits[] = {
        0x8000000000000000ULL, 0x7FF8000000001234ULL, 0xFFF8000000005678ULL,
        0x7FF0000000000000ULL, 0xFFF0000000000000ULL, 0x0000000000000001ULL,
    };
    for (std::size_t i = 0; i < sizeof(equity_bits) / sizeof(equity_bits[0]); ++i) {
        auto& point = points[static_cast<std::size_t>(points_per_window) - 2 + i];
        std::memcpy(&point.equity, &equity_bits[i], sizeof(point.equity));
        std::memcpy(&point.open_profit,
                    &equity_bits[(i + 1) % (sizeof(equity_bits) / sizeof(equity_bits[0]))],
                    sizeof(point.open_profit));
    }
    MemoryScratch file(points);
    const SpillCurve source(file.reader());
    for (std::int64_t i = 0; i < source.size(); ++i) {
        CHECK(same_point(source[i], points[static_cast<std::size_t>(i)]));
    }
    CHECK(same_point(source[0], points.front()));
    file.fail_next = true;
    CHECK(throws_as<std::system_error>([&] { (void)source[points_per_window]; }));
    const auto after_failure = file.reads;
    // A failed refill copied another window's first point over index 0.
    CHECK(same_point(source[0], points.front()));
    CHECK(file.reads == after_failure + 1);
    CHECK(same_point(source[points_per_window],
                     points[static_cast<std::size_t>(points_per_window)]));
}

class TempScratch {
public:
    TempScratch() : file_(std::tmpfile()) {
        if (file_ == nullptr) throw std::system_error(errno, std::generic_category(), "tmpfile");
    }
    ~TempScratch() { std::fclose(file_); }
    TempScratch(const TempScratch&) = delete;
    TempScratch& operator=(const TempScratch&) = delete;

    int descriptor() const { return ::fileno(file_); }
    void append(const void* bytes, std::size_t count) {
        if (std::fwrite(bytes, 1, count, file_) != count) {
            throw std::system_error(errno, std::generic_category(), "write test scratch");
        }
    }
    void flush() {
        if (std::fflush(file_) != 0) {
            throw std::system_error(errno, std::generic_category(), "flush test scratch");
        }
    }

private:
    std::FILE* file_;
};

void descriptor_adapter_matches_array() {
    auto points = random_curve(3 * points_per_window + 17, 0x521ED7ULL, 86400000, false);
    points.back().open_profit = -0.0;
    TempScratch file;
    const unsigned char prefix[13] = {};
    const unsigned char suffix[11] = {};
    file.append(prefix, sizeof(prefix));
    file.append(points.data(), points.size() * sizeof(points[0]));
    file.append(suffix, sizeof(suffix));
    file.flush();
    const auto cursor = ::lseek(file.descriptor(), 0, SEEK_CUR);
    CHECK(cursor >= 0);
    const auto n = static_cast<std::int64_t>(points.size());
    const ScratchCurveRegion original{file.descriptor(), sizeof(prefix), n};
    {
        auto argument = original;
        const SpillCurve source(argument);
        argument = {-1, 0, 0}; // constructor must retain its OWN region descriptor
        const SpillCurve independent(original);
        for (std::int64_t i = 0; i < n; ++i) {
            CHECK(same_point(source[i], points[static_cast<std::size_t>(i)]));
            CHECK(same_point(independent[n - i - 1],
                             points[static_cast<std::size_t>(n - i - 1)]));
        }
        const ArrayCurve array(points.data(), n);
        for (const std::string zone : {"UTC", "America/New_York"}) {
            const auto want = equity_stats_reference::compute_equity_stats(
                points.data(), n, initial_capital, zone, 100.0, 105.0, n / 2, 500.0);
            const auto from_array = compute_equity_stats<ArrayCurve>(
                array, n, initial_capital, zone, 100.0, 105.0, n / 2, 500.0);
            const auto from_file = compute_equity_stats<SpillCurve>(
                source, n, initial_capital, zone, 100.0, 105.0, n / 2, 500.0);
            check_stats(from_array, want, "descriptor array/reference", n, zone);
            check_stats(from_file, want, "descriptor spill/reference", n, zone);
        }
    }
    CHECK(::lseek(file.descriptor(), 0, SEEK_CUR) == cursor);
    CHECK(::fcntl(file.descriptor(), F_GETFD) != -1); // source borrows the descriptor
}

void invalid_regions_bounds_and_short_files() {
    const SpillCurve empty(ScratchCurveReader{});
    CHECK(empty.size() == 0);
    CHECK(throws_as<std::out_of_range>([&] { (void)empty[0]; }));
    const SpillCurve empty_region(ScratchCurveRegion{-1, 0, 0});
    CHECK(empty_region.size() == 0);
    CHECK(throws_as<std::invalid_argument>([] {
        const SpillCurve source(ScratchCurveReader{nullptr, 1, nullptr});
    }));
    CHECK(throws_as<std::invalid_argument>([] {
        const SpillCurve source(ScratchCurveReader{nullptr, -1, nullptr});
    }));
    CHECK(throws_as<std::invalid_argument>([] {
        const SpillCurve source(ScratchCurveRegion{-1, 0, 1});
    }));
    CHECK(throws_as<std::invalid_argument>([] {
        const SpillCurve source(ScratchCurveRegion{0, -1, 1});
    }));
    CHECK(throws_as<std::invalid_argument>([] {
        const SpillCurve source(ScratchCurveRegion{0, 0, -1});
    }));
    CHECK(throws_as<std::invalid_argument>([] {
        const SpillCurve source(ScratchCurveRegion{
            0, std::numeric_limits<std::int64_t>::max() - 23, 1});
    }));
    CHECK(throws_as<std::invalid_argument>([] {
        const SpillCurve source(ScratchCurveRegion{
            0, 0, std::numeric_limits<std::int64_t>::max()});
    }));

    const std::vector<pf_equity_point_t> points = {{1704067200000LL, 10000.0, 0.0}};
    MemoryScratch memory(points);
    const SpillCurve source(memory.reader());
    CHECK(throws_as<std::out_of_range>([&] { (void)source[-1]; }));
    CHECK(throws_as<std::out_of_range>([&] { (void)source[source.size()]; }));
    CHECK(memory.reads == 0);

    TempScratch truncated;
    truncated.append(points.data(), sizeof(points[0]) - 1);
    truncated.flush();
    const SpillCurve short_source(ScratchCurveRegion{truncated.descriptor(), 0, 1});
    for (int retry = 0; retry < 2; ++retry) {
        try {
            (void)short_source[0];
            CHECK(false);
        } catch (const std::system_error& error) {
            CHECK(error.code() == std::error_code(EIO, std::generic_category()));
            CHECK(pineforge::classify_run_failure(error).code ==
                  pineforge::RunFailureCode::report_sink_failed);
        }
    }
}

} // namespace

int main() {
    try {
        random_metrics_match_frozen_reference();
        month_boundaries_at_window_edges();
        cache_edges_and_repeated_walks();
        exact_payloads_and_failed_refills();
        descriptor_adapter_matches_array();
        invalid_regions_bounds_and_short_files();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "unexpected spill-curve exception: %s\n", error.what());
        return 1;
    }
    std::printf("metrics SpillCurve: %lld checks, %d failures\n",
                static_cast<long long>(checks), failures);
    return failures == 0 ? 0 : 1;
}
