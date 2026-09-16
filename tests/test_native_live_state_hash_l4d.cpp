// Native-route state-hash twin for the observable live-state family. It does
// not inspect a retired owner book; it changes an actual source command and
// checks the resulting broker-state projection.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdio>
#include <string>
#include <vector>

using namespace pineforge;

namespace {
int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expression); ++failures; } } while (false)
Bar flat(double price, std::int64_t timestamp) { return {price, price, price, price, 1, timestamp}; }
class HashProbe final : public source::PineStrategyHost {
public:
    explicit HashProbe(std::string id) : id_(std::move(id)) {}
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry(id_, true, na<double>(), na<double>(), 1.0);
    }
private:
    std::string id_;
};
}

int main() {
    const std::vector<Bar> bars = {flat(100, 0), flat(101, 60'000)};
    HashProbe same_a("A");
    HashProbe same_b("A");
    HashProbe changed("B");
    strategy_set_broker_state_hash_recording(&same_a, 1);
    strategy_set_broker_state_hash_recording(&same_b, 1);
    strategy_set_broker_state_hash_recording(&changed, 1);
    same_a.run(bars.data(), static_cast<int>(bars.size()));
    same_b.run(bars.data(), static_cast<int>(bars.size()));
    changed.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(same_a.last_error().empty() && same_b.last_error().empty() && changed.last_error().empty());
    CHECK(strategy_broker_state_hash(&same_a) == strategy_broker_state_hash(&same_b));
    CHECK(strategy_broker_state_hash(&same_a) != strategy_broker_state_hash(&changed));
    std::printf("native live-state-hash twin: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
