// L9c: the scalar broker_state_hash() is the final script-point fingerprint
// and must not depend on the waived recording switch.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace pineforge;

namespace {

int failures = 0;

#define CHECK(cond) do {                                                        \
    if (!(cond)) {                                                              \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
        ++failures;                                                             \
    }                                                                           \
} while (0)

Bar flat(double price, std::int64_t timestamp) {
    return {price, price, price, price, 1.0, timestamp};
}

class HashProbe final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 1) strategy_entry("L", true);
        if (bar_index_ == 4) strategy_close_all();
    }
};

}  // namespace

int main() {
    std::vector<Bar> bars;
    for (int i = 0; i < 8; ++i) bars.push_back(flat(100.0 + i, i * 60'000LL));

    HashProbe recorded;
    recorded.set_broker_state_hash_recording(true);
    recorded.run(bars.data(), static_cast<int>(bars.size()));
    HashProbe unrecorded;
    unrecorded.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(recorded.last_error().empty());
    CHECK(unrecorded.last_error().empty());
    CHECK(recorded.broker_state_hash() == unrecorded.broker_state_hash());
    CHECK(recorded.broker_state_hash() != 0);

    ReportC report{};
    recorded.fill_report(&report);
    CHECK(report.broker_state_hash_len == 8);
    CHECK(report.broker_state_hash != nullptr);
    CHECK(report.broker_state_hash[7] == recorded.broker_state_hash());
    CHECK(report.broker_state_hash[7] == unrecorded.broker_state_hash());
    BacktestEngine::free_report(&report);

    std::printf("test_l9c_broker_hash_recording_independent: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
