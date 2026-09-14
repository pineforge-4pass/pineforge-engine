// Native-route twin for the F7 literal in tests/oracle/test_oracle_reversal.cpp.
#include <pineforge/source/pine_native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace pineforge;
namespace {
int checks = 0;
int failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

class FlipProbe final : public source::PineNativeHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("old", true, kNaN, kNaN, 1.0);
        if (bar_index_ == 1) strategy_entry("flip", false, kNaN, kNaN, 0.1);
    }
private:
    static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
};
} // namespace

int main() {
    FlipProbe probe;
    const Bar bars[] = {
        {100, 100, 100, 100, 1, 1000},
        {100, 100, 100, 100, 1, 2000},
        {90, 90, 90, 90, 1, 3000},
        {90, 90, 90, 90, 1, 4000},
    };
    probe.run(bars, 4);
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    CHECK(bits(probe.physical_position().signed_units) == UINT64_C(0xbfb999999999999a));
    if (probe.trade_count() == 1) {
        const Trade& row = probe.get_trade(0);
        CHECK(bits(row.qty) == UINT64_C(0x3ff0000000000000));
        CHECK(row.entry_id == "old" && row.exit_id == "flip");
    }
    std::printf("R4-D native F7 reversal twin: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
