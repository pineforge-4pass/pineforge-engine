// Native-route twin for the public, byte-stable part of the live pending-row
// mirror. Owner-private cancellation construction is intentionally not used.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pineforge;

namespace {
int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expression); ++failures; } } while (false)

std::uint64_t fnv1a64(const std::string& value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : value) { hash ^= byte; hash *= 1099511628211ULL; }
    return hash;
}
Bar flat(double price, std::int64_t timestamp) { return {price, price, price, price, 1, timestamp}; }
const std::string kLongId(70, 'x');

class MirrorProbe final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 1) strategy_exit(kLongId, "L", na<double>(), 95.0);
    }
};
}

int main() {
    const std::vector<Bar> bars = {flat(100, 0), flat(100, 60'000)};
    MirrorProbe probe;
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(strategy_pending_orders_len(&probe) == 1);
    pf_pending_order_v1_t row{};
    CHECK(strategy_pending_order_get(&probe, 0, &row, sizeof(row)) == 0);
    CHECK(row.struct_version == PF_PENDING_ORDER_STRUCT_VERSION && row.size == sizeof(row));
    CHECK(row.id_truncated == 1 && std::strlen(row.id) == 63);
    CHECK(std::string(row.id) == kLongId.substr(0, 63));
    CHECK(row.id_hash64 == fnv1a64(kLongId));
    CHECK(std::strcmp(row.from_entry, "L") == 0 && row.from_entry_truncated == 0);
    CHECK(row.from_entry_hash64 == fnv1a64("L"));
    CHECK(row.stop_price == 95.0 && row.is_long == 0);
    pf_pending_order_v1_t repeat{};
    CHECK(strategy_pending_order_get(&probe, 0, &repeat, sizeof(repeat)) == 0);
    CHECK(std::memcmp(&row, &repeat, sizeof(row)) == 0);
    std::printf("native pending-mirror twin: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
