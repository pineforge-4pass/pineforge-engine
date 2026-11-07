// Expected red on a3e7822e: the map-policy export is absent. With plumbing
// alone, the callback's old refusal still lacks the exact TV context/code.
#include "p2_a5strmap_fixture.hpp"
#include "cr559_map_capacity_fixture.hpp"

#include <pineforge/source/pine_strategy_host.hpp>

namespace {
class RefusalHost final : public pineforge::source::PineStrategyHost {
public:
    explicit RefusalHost(bool deferred) {
        a5_fixture::configure(*this);
        if (!deferred) map_ = cr559::Map::new_();
    }
    void on_source_bar(const pineforge::Bar&) override {
        ++callbacks;
        // The run must reinstall the host's captured policy after the
        // harness has reset the thread default following construction.
        if (map_.is_na()) map_ = cr559::Map::new_();
        for (cr559::Scalar index = 0; index < 50000; ++index) {
            (void)map_.put(cr559::key_for(index), cr559::value_for(index));
            ++accepted;
        }
        cr559::require(map_.put(cr559::key_for(31), -67) == cr559::value_for(31),
                       "real callback replacement failed at the configured maximum");
        (void)map_.put(cr559::key_for(50000), 97);
        reached_after_refusal = true;
        strategy_entry("must-not-run", true, pineforge::na<double>(),
                       pineforge::na<double>(), 1.0);
    }
    void verify() const {
        cr559::require(callbacks == 1 && accepted == 50000 && !reached_after_refusal,
                       "map refusal did not stop the first bar at distinct pair 50001");
        cr559::require(map_.size() == 50000 && map_.get(cr559::key_for(31)) == -67 &&
                           !map_.contains(cr559::key_for(50000)),
                       "real callback refusal mutated map membership/value");
    }

    int callbacks = 0;
    cr559::Scalar accepted = 0;
    bool reached_after_refusal = false;

private:
    cr559::Map map_;
};
} // namespace

int main() {
    try {
        cr559::Session session;
        cr559::LimitScope policy(50000);
        RefusalHost constructed(false);
        RefusalHost deferred(true);
        policy.unlimited();
        const auto bars = a5_fixture::bars(3);
        for (auto* host : {&constructed, &deferred}) {
            host->run(bars.data(), static_cast<int>(bars.size()));
            host->verify();
            auto* handle = static_cast<pineforge::BacktestEngine*>(host);
            cr559::require(strategy_last_run_status(handle) == 1,
                           "real callback capacity refusal did not fail the run");
            const char* code = strategy_get_last_error_code(handle);
            const char* args = strategy_get_last_error_args(handle);
            const char* text = strategy_get_last_error(handle);
            cr559::require(code && std::string(code) == "pine_map_capacity_exceeded",
                           "C run-failure getter lost the map-capacity catalog code");
            cr559::require(args && std::string(args) == cr559::capacity_args(50000, 50001, 0),
                           "C run-failure getter lost exact RE10070 context");
            cr559::require(text && std::string(text) == cr559::capacity_message(50000, 50001, 0),
                           "C run-failure getter changed exact TradingView English");
            cr559::require(host->trade_count() == 0, "execution continued after the map refusal");
        }
        std::puts("CR559 run: real callback stops at bar zero with typed C failure getters");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CR559 run FAIL: %s\n", error.what());
        return 1;
    }
}
