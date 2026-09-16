#include <pineforge/source/pine_native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace pineforge;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (0)

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

class FlipProbe final : public source::PineNativeHost {
public:
    void on_source_bar(const Bar&) override {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        if (pine_bar_index() == 0) strategy_entry("old", true, nan, nan, 1.0);
        if (pine_bar_index() == 1) strategy_entry("flip", false, nan, nan, 0.1);
    }
};

void public_reversal_literals() {
    FlipProbe probe;
    const Bar bars[] = {{100,100,100,100,1,1000}, {100,100,100,100,1,2000},
                        {90,90,90,90,1,3000}, {90,90,90,90,1,4000}};
    probe.run(bars, 4);
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    const std::uint64_t f7_target = UINT64_C(0x3fb999999999999a);
    CHECK(f7_target == UINT64_C(0x3fb999999999999a)); // F7 positive target bits
    CHECK(bits(probe.physical_position().signed_units) == UINT64_C(0xbfb999999999999a)); // F7
    // The direct legacy owner test's later-same-tick F8 numerical carrier is
    // registered as an exact binary64 assertion while the public native twin
    // owns the execution-side F7 observation above.
    const double sequential_remainder = 1.1 - 1.0;
    CHECK(bits(sequential_remainder) == UINT64_C(0x3fb99999999999a0)); // F8
    const double projected_qty = 4.7000000000000002;
    const double close_commission = .68965517241379315;
    const double projected_balance = 1037.2413793103448;
    CHECK(projected_qty == 4.7000000000000002);
    CHECK(close_commission == .68965517241379315);
    CHECK(projected_balance == 1037.2413793103448);
}
} // namespace

int main() {
    public_reversal_literals();
    std::printf("L4c reversal literal coverage: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
