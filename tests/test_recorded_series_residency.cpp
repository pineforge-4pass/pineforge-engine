// Uses only the previous host API, so the heap bound is a runtime red on the
// previous implementation, independently of the new reader declarations.
#include <pineforge/source/pine_strategy_host.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace tape_heap {
std::atomic<long long> live{0};
std::atomic<long long> peak{0};
constexpr std::size_t header = alignof(std::max_align_t);

void* take(std::size_t bytes) {
    if (bytes > static_cast<std::size_t>(-1) - header) return nullptr;
    void* raw = std::malloc(bytes + header);
    if (!raw) return nullptr;
    *static_cast<std::size_t*>(raw) = bytes;
    const auto now = live.fetch_add(static_cast<long long>(bytes)) + static_cast<long long>(bytes);
    auto high = peak.load();
    while (now > high && !peak.compare_exchange_weak(high, now)) {}
    return static_cast<char*>(raw) + header;
}
void give(void* value) {
    if (!value) return;
    void* raw = static_cast<char*>(value) - header;
    live.fetch_sub(static_cast<long long>(*static_cast<std::size_t*>(raw)));
    std::free(raw);
}
} // namespace tape_heap

void* operator new(std::size_t n) {
    if (auto* p = tape_heap::take(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (auto* p = tape_heap::take(n)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return tape_heap::take(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return tape_heap::take(n);
}
void operator delete(void* p) noexcept { tape_heap::give(p); }
void operator delete[](void* p) noexcept { tape_heap::give(p); }
void operator delete(void* p, std::size_t) noexcept { tape_heap::give(p); }
void operator delete[](void* p, std::size_t) noexcept { tape_heap::give(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { tape_heap::give(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { tape_heap::give(p); }

namespace {
using namespace pineforge;
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; \
} } while (false)

class TapeHost final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {}
    double read(std::int64_t open) {
        current_bar_.timestamp = open;
        return recorded_series_value("tape");
    }
    std::uint64_t data_digest() const { return symbol_data_digest_; }
};

bool same_bits(double a, double b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}
} // namespace

int main() {
    constexpr int rows = 300000;
    constexpr std::int64_t first = 1735689600000LL;
    std::vector<std::int64_t> opens(rows);
    std::vector<double> values(rows);
    for (int i = 0; i < rows; ++i) {
        opens[i] = first + static_cast<std::int64_t>(i) * 60000;
        values[i] = (i % 997 - 498) * 0.125;
    }
    values[0] = -0.0;
    // A noncanonical NaN payload must survive storage as exact bytes.
    const std::uint64_t nan_bits = 0x7ff8000000000042ULL;
    std::memcpy(&values[rows / 2], &nan_bits, sizeof(nan_bits));

    TapeHost host;
    BrokerStateHashSink expected;
    expected.u(0); // feeds
    expected.u(0); // facts
    expected.u(1); // tapes
    expected.s("tape");
    expected.u(rows);
    for (int i = 0; i < rows; ++i) {
        expected.i(opens[i]);
        expected.d(values[i]);
    }
    const auto before = tape_heap::live.load();
    tape_heap::peak.store(before);
    CHECK(host.set_recorded_series("tape", opens.data(), values.data(), rows));
    const auto retained_bytes = tape_heap::live.load() - before;
    std::printf("recorded tape retained heap: %lld bytes for %d rows\n",
                retained_bytes, rows);
    // 256 KiB optional cache plus 64 KiB for all fixed owner/allocator state.
    // The old two vectors retain 4,800,000 payload bytes and fail this row.
    CHECK(retained_bytes <= 320 * 1024);
    CHECK(tape_heap::peak.load() - before <= 320 * 1024);
    CHECK(host.data_digest() == expected.h);
    for (int index : {0, rows - 1, rows / 2, 1, rows - 2, 0}) {
        CHECK(same_bits(host.read(opens[index]), values[index]));
    }
    const double saved_first = values[0], saved_last = values.back();
    const auto last_open = opens.back();
    values.front() = 12345.0;
    values.back() = 67890.0;
    opens.front() = -1;
    opens.back() = -2;
    CHECK(same_bits(host.read(first), saved_first));
    CHECK(same_bits(host.read(last_open), saved_last));
    CHECK(std::isnan(host.read(first - 60000)));
    CHECK(std::isnan(host.read(first + 1)));
    CHECK(std::isnan(host.read(last_open + 60000)));
    CHECK(host.set_recorded_series("tape", nullptr, nullptr, 0));
    CHECK(std::isnan(host.read(first)));

    constexpr int days = 100000;
    std::vector<std::int64_t> stamps(days);
    for (int i = 0; i < days; ++i)
        stamps[i] = first + static_cast<std::int64_t>(i) * kMsPerDay;
    NativeDayPartition partition;
    const auto calendar_before = tape_heap::live.load();
    tape_heap::peak.store(calendar_before);
    CHECK(build_native_day_partition(partition, "UTC", "24x7", stamps, nullptr, 0));
    // Stamps intentionally remain owned; the three derived columns must fit
    // three bounded readers, even during construction.
    constexpr long long calendar_bound = days * sizeof(std::int64_t)
        + 3 * 256 * 1024 + 64 * 1024;
    CHECK(tape_heap::live.load() - calendar_before <= calendar_bound);
    CHECK(tape_heap::peak.load() - calendar_before <= calendar_bound);
    for (int index : {0, days - 1, days / 2, 1, days - 2, 0}) {
        NativeDayPartitionScope scope(&partition);
        CHECK(session_day_index(stamps[index], "UTC", "24x7")
              == stamps[index] / kMsPerDay);
    }
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
