// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

// Literal lifecycle contract test. Compiled Pine/indicator reuse is a separate
// Cloud diagnostic: this test establishes the engine-owned dispatch boundary.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cassert>
#include <stdexcept>
#include <vector>

using namespace pineforge;

class ScriptProbe final : public pineforge::source::PineStrategyHost {
public:
    int preparations = 0;
    int configurations = 0;
    int value = -999;
    bool prepared = false;
    bool allow_precalc = false;
    bool fail_preparation = false;
    std::vector<int> observed;

    ScriptProbe() { initial_capital_ = 12345.0; }

    void prepare_script_run(const Bar*, int, bool allow) override {
        ++preparations;
        prepared = true;
        allow_precalc = allow;
        observed.clear();
        value = std::stoi(inputs_.at("seed"));
        assert(trades_.empty());
        assert(signed_position_size() == 0.0);
        assert(initial_capital_ == 12345.0);
        if (fail_preparation) throw std::runtime_error("literal preparation failure");
    }

    void configure_security_evaluators() override {
        assert(prepared);
        assert(observed.empty());
        assert(value == std::stoi(inputs_.at("seed")));
        ++configurations;
    }

    void on_source_bar(const Bar&) override {
        assert(prepared);
        observed.push_back(++value);
    }
};

class CycleProbe final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ % 3 == 0)
            strategy_entry("L", true, na<double>(), na<double>(), 1.0);
        if (bar_index_ % 3 == 1) strategy_close_all();
    }
    int64_t next_cycle() const { return next_position_cycle_seq_; }
    // The legacy source sequence is retired.  Incarnation is the public
    // native ordering identity and is reset at the same run boundary.
    int64_t next_order_sequence() const {
        return static_cast<int64_t>(next_order_incarnation_);
    }
    uint64_t next_incarnation() const { return next_order_incarnation_; }
    const std::vector<uint64_t>& hashes() const { return broker_state_hashes_; }
    void seed_prior_run_snapshots() {
        // Seed only generic broker state.  The deleted source scheduler's
        // private snapshots deliberately have no test backdoor; run() must
        // still reset the native identities and hash to a clean empty run.
        position_cycle_seq_ = 7;
        next_position_cycle_seq_ = 8;
        next_order_incarnation_ = 9;
        stream_action_sequence_ = 10;
    }
};

int main() {
    const Bar bars[] = {
        {10, 11, 9, 10, 1, 60000},
        {11, 12, 10, 11, 2, 120000},
        {12, 13, 11, 12, 3, 180000},
    };
    ScriptProbe p;
    p.set_input("seed", "7");
    // Enter via the base API, as stream_begin and other native callers do.
    BacktestEngine& base = p;
    base.run(bars, 1);
    assert(p.preparations == 1 && p.allow_precalc);
    assert((p.observed == std::vector<int>{8}));
    base.run(bars, 3);
    assert(p.preparations == 2 && p.allow_precalc);
    assert((p.observed == std::vector<int>{8, 9, 10}));

    const Bar cycle_bars[] = {
        {10, 10, 10, 10, 1, 60000}, {11, 11, 11, 11, 1, 120000},
        {12, 12, 12, 12, 1, 180000}, {13, 13, 13, 13, 1, 240000},
        {14, 14, 14, 14, 1, 300000}, {15, 15, 15, 15, 1, 360000},
    };
    CycleProbe fresh_cycles, reused_cycles;
    fresh_cycles.set_broker_state_hash_recording(true);
    reused_cycles.set_broker_state_hash_recording(true);
    fresh_cycles.run(cycle_bars, 6);
    reused_cycles.run(cycle_bars, 6);
    reused_cycles.run(cycle_bars, 6);
    // Two separate opens within each run consume two distinct cycle IDs.
    assert(fresh_cycles.next_cycle() == 3);
    assert(reused_cycles.next_cycle() == fresh_cycles.next_cycle());
    assert(fresh_cycles.next_order_sequence() == 5);
    assert(reused_cycles.next_order_sequence() == fresh_cycles.next_order_sequence());
    assert(fresh_cycles.next_incarnation() == 5);
    assert(reused_cycles.next_incarnation() == fresh_cycles.next_incarnation());
    assert(fresh_cycles.hashes().size() == 6);
    assert(reused_cycles.hashes().size() == 6);
    assert(reused_cycles.hashes() == fresh_cycles.hashes());

    CycleProbe fresh_empty, previous_snapshots;
    fresh_empty.run(nullptr, 0);
    previous_snapshots.seed_prior_run_snapshots();
    assert(previous_snapshots.broker_state_hash() != fresh_empty.broker_state_hash());
    previous_snapshots.run(nullptr, 0);
    assert(previous_snapshots.broker_state_hash() == fresh_empty.broker_state_hash());

    p.prepared = false;
    base.run(bars, 3, "1", "1");
    assert(p.preparations == 3 && !p.allow_precalc);
    assert(p.configurations == 1);
    assert((p.observed == std::vector<int>{8, 9, 10}));
    base.run(bars, 3, "", "");
    assert(p.preparations == 4 && p.allow_precalc);
    base.run(bars, 3, "", "1");
    assert(p.preparations == 5 && !p.allow_precalc);
    base.run(bars, 3, "1", "");
    assert(p.preparations == 6 && !p.allow_precalc);
    base.run(bars, 3, "", "", true);
    assert(p.preparations == 7 && !p.allow_precalc);

    // A changed input persists and is resolved afresh, not reset to defaults.
    p.set_input("seed", "19");
    base.run(bars, 2, "1", "1");
    assert((p.observed == std::vector<int>{20, 21}));
    base.run(nullptr, 0);
    assert(p.observed.empty() && p.value == 19);
    const int before_failure = p.preparations;
    p.fail_preparation = true;
    base.run(bars, 3);
    assert(p.preparations == before_failure + 1);
    assert(p.observed.empty());
    p.fail_preparation = false;
    base.run(bars, 2);
    assert((p.observed == std::vector<int>{20, 21}));

    p.set_input("seed", "7");
    const int before_stream = p.preparations;
    assert(base.stream_begin(bars, 2, "1", "1"));
    assert(p.preparations == before_stream + 1 && !p.allow_precalc);
    assert((p.observed == std::vector<int>{8, 9}));
    assert(base.stream_push_tick(TradeTick{180000, 1, 12, 1}));
    assert(base.stream_advance_time(240000));
    assert(p.preparations == before_stream + 1);
    assert(p.observed.size() >= 3 && p.observed[2] == 10);
    assert(base.stream_end());
    assert(p.preparations == before_stream + 1);

    assert(base.stream_begin(bars, 2, "1", "1"));
    assert(p.preparations == before_stream + 2);
    assert((p.observed == std::vector<int>{8, 9}));
    assert(base.stream_end());
    base.run(bars, 3);
    assert(p.preparations == before_stream + 3 && p.allow_precalc);
    assert((p.observed == std::vector<int>{8, 9, 10}));
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
