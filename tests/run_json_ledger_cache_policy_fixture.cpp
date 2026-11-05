// Real-runtime wrapper projection fixture, compatible with 12148 headers.
// No substitute policy parser or cache implementation lives in this module.
#include "p2cx_tledger_fixture.hpp"
#include "trade_ledger_cache_io_fixture.hpp"
#include <pineforge/pineforge.h>

namespace {
using namespace p2cx_tledger;
class Fixture final : public Reader {
public:
    std::size_t population = 0;
    std::uint64_t run_number = 0;
    void on_native_run_begin() override {
        for (std::size_t i = 0; i < population; ++i)
            trades_.push_back(row(static_cast<int>(i)));
        for (std::size_t i = 0; i < population; ++i)
            (void)trades_.read(i);
    }
};
}

extern "C" {
pf_strategy_t ledger_fixture_create() { return new Fixture; }
void ledger_fixture_free(pf_strategy_t state) { delete static_cast<Fixture*>(state); }
int ledger_fixture_policy(pf_strategy_t state, const pf_cache_policy_v1* policy) {
    return strategy_set_cache_policy_v1(state, policy);
}
int ledger_fixture_run(pf_strategy_t state, std::uint64_t population) {
    auto& host = *static_cast<Fixture*>(state);
    host.population = static_cast<std::size_t>(population);
    auto settings = spec("ledger-wrapper");
    settings.identity.run_number = ++host.run_number;
    if (host.configure_native(settings).status != NativeSetupStatus::Applied)
        return -1;
    const Bar bar{100, 101, 99, 100, 1, T};
    host.run(&bar, 1);
    return host.last_run_status();
}
std::uint64_t ledger_fixture_initial_rows() { return TradeLedger::kDefaultCacheRows; }
// The rows an omitted ledger setting holds: the provisional byte default's
// geometry, derived independently of the product (ledger_default_cache_rows.hpp).
std::uint64_t ledger_fixture_default_rows() { return ledger_oracle::default_cache_rows(); }
std::uint64_t ledger_fixture_capacity(std::uint64_t rows) {
    return TradeLedger::cache_capacity_bytes(static_cast<std::size_t>(rows));
}
std::uint64_t ledger_fixture_rows(pf_strategy_t state) {
    return static_cast<Fixture*>(state)->ledger().cache_rows();
}
std::uint64_t ledger_fixture_read(pf_strategy_t state, std::uint64_t index) {
    trade_ledger_cache_io::Measure measure;
    const auto actual = static_cast<Fixture*>(state)->ledger().read(static_cast<std::size_t>(index));
    same_trade(actual, row(static_cast<int>(index)));
    const auto work = measure.finish();
    return failures == 0 ? work.calls : UINT64_MAX;
}
}
