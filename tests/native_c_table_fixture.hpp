// R5 lane D2-A: a seeded C host whose callback table varies by the hooks it
// installs, for the witnesses of the hook declarations.
//
// A C host declares its hooks from its pf_native_callbacks_v1 when it is
// created: a table without on_bar_open declares no bar-open hook, one without
// on_precommit no precommit hook (src/native_c_host.cpp). This fixture runs
// one seeded strategy -- market, limit and stop transactions of either side,
// flattens and explicit reduces, cancels, requests born in the fill callback
// -- through tables that differ only in those hooks: absent, or installed and
// answering what the kernel does without them (on_bar_open returns 0,
// on_precommit answers PF_NATIVE_ANSWER_DEFAULT). It can also install an
// on_lot_excursion whose answer depends on how often it has been asked, so a
// run that consulted it a different number of times books a different row.
// What a run produced is kept as values: the continuation at every
// calculation and applied fill, the final continuation and broker hashes,
// every closed row (excursions included), every event, the hook counts, the
// consumer's recorded declarations and the settlement previews it built.
//
// Source-free: kernel-only builds register the rows that use it.
#pragma once

#include "../src/engine_internal.hpp"
#include "../src/native_execution_consumer.hpp"
#include "native_match_book_fixture.hpp"

#include <pineforge/native_c_api.h>
#include <pineforge/pineforge.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace c_table {

using namespace pineforge;

struct Hooks {
    bool bar_open = false;
    bool precommit = false;
    bool lot_excursion = false;
};

struct Outcome {
    std::vector<std::uint64_t> trace;
    std::uint64_t continuation = 0;
    std::uint64_t broker = 0;
    std::uint64_t trades_digest = 0;
    std::uint64_t events_digest = 0;
    std::size_t events = 0;
    int trades = 0;
    long bars = 0;
    long applied = 0;
    long bar_opens = 0;
    long precommits = 0;
    long excursions = 0;
    std::uint64_t previews = 0;
    bool declared_bar_open = true;
    int rc = -1;
    bool completed = false;
};

struct State {
    pf_strategy_t handle = nullptr;
    k3_book::Rng rng{1};
    std::vector<std::uint64_t> live;
    Outcome outcome;
};

inline void trace(State& state) {
    std::uint64_t value = 0;
    strategy_native_continuation_hash_v1(state.handle, &value);
    state.outcome.trace.push_back(value);
}

inline void place(State& state, long ref) {
    pf_native_request_v1 request;
    std::memset(&request, 0, sizeof(request));
    request.struct_size = static_cast<std::uint32_t>(sizeof(request));
    request.version = PF_NATIVE_API_VERSION;
    const bool buy = state.rng.percent(50);
    const int intent = state.rng.below(100);
    if (intent < 65) {
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        const double units = static_cast<double>(state.rng.between(1, 3));
        request.intent_value = buy ? units : -units;
    } else if (intent < 80) {
        request.intent = PF_NATIVE_INTENT_FLATTEN;
    } else {
        request.intent = PF_NATIVE_INTENT_REDUCE;
        request.reduce_size = PF_NATIVE_REDUCE_EXPLICIT_UNITS;
        request.intent_value = 1.0;
    }
    const long away = state.rng.percent(15) ? -state.rng.between(1, 4) : state.rng.between(1, 30);
    const int trigger = state.rng.below(100);
    if (trigger < 30) {
        request.trigger = PF_NATIVE_TRIGGER_MARKET;
    } else if (trigger < 65) {
        request.trigger = PF_NATIVE_TRIGGER_LIMIT;
        request.p1 = k3_book::ticks(buy ? ref - away : ref + away);
    } else {
        request.trigger = PF_NATIVE_TRIGGER_STOP;
        request.p1 = k3_book::ticks(buy ? ref + away : ref - away);
    }
    request.label = "c-table";
    std::uint64_t incarnation = 0;
    if (strategy_native_submit_v1(state.handle, &request, &incarnation, nullptr) == PF_NATIVE_OK) {
        state.live.push_back(incarnation);
    }
}

inline int on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1*) {
    auto& state = *static_cast<State*>(user);
    ++state.outcome.bars;
    trace(state);
    const long ref = static_cast<long>(bar->close * 4.0);
    if (!state.live.empty() && state.rng.percent(30)) {
        const std::size_t at =
            static_cast<std::size_t>(state.rng.below(static_cast<int>(state.live.size())));
        strategy_native_cancel_v1(state.handle, state.live[at]);
        state.live.erase(state.live.begin() + static_cast<std::ptrdiff_t>(at));
    }
    const int count = state.rng.between(0, 2);
    for (int i = 0; i < count; ++i) place(state, ref);
    return 0;
}

inline int on_applied(void* user, const pf_native_applied_v1* applied,
                      const pf_native_decision_v1*) {
    auto& state = *static_cast<State*>(user);
    ++state.outcome.applied;
    trace(state);
    if (state.rng.percent(30)) place(state, static_cast<long>(applied->resolved_price * 4.0));
    return 0;
}

inline int on_bar_open(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    ++static_cast<State*>(user)->outcome.bar_opens;
    return 0;
}

inline int on_precommit(void* user, const pf_native_precommit_view_v1*, std::uint32_t*) {
    ++static_cast<State*>(user)->outcome.precommits;
    return PF_NATIVE_ANSWER_DEFAULT;
}

inline int on_lot_excursion(void* user, const pf_native_lot_excursion_v1* facts,
                            double* favorable, double* adverse) {
    auto& state = *static_cast<State*>(user);
    ++state.outcome.excursions;
    // Depends on how often it has been asked, as PERF-L2's owner does.
    const double asked = static_cast<double>(state.outcome.excursions % 7);
    const double move = (facts->fill_price - facts->entry_price) * facts->closed_qty;
    *favorable = std::abs(move) + asked;
    *adverse = std::abs(move) * 0.5 + asked;
    return 1;
}

inline Outcome run(const k3_book::Tape& tape, std::uint64_t seed, Hooks hooks) {
    State state;
    state.rng = k3_book::Rng(seed);
    pf_native_callbacks_v1 table;
    std::memset(&table, 0, sizeof(table));
    table.struct_size = static_cast<std::uint32_t>(sizeof(table));
    table.version = PF_NATIVE_API_VERSION;
    table.user = &state;
    table.on_bar = &on_bar;
    table.on_applied = &on_applied;
    if (hooks.bar_open) table.on_bar_open = &on_bar_open;
    if (hooks.precommit) table.on_precommit = &on_precommit;
    if (hooks.lot_excursion) table.on_lot_excursion = &on_lot_excursion;
    state.handle = strategy_native_host_create_v1(&table);
    if (!state.handle) return state.outcome;

    pf_native_run_spec_v1 spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.struct_size = static_cast<std::uint32_t>(sizeof(spec));
    spec.session_key = "d2a-c-table";
    spec.run_number = 1;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "D2A";
    spec.tickerid = "TEST:D2A";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.description = "";
    spec.volumetype = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "";
    spec.initial_capital = 1.0e6;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.25;
    spec.fee_kind = PF_NATIVE_FEE_PERCENT;
    spec.fee_value = 0.05;
    spec.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;
    if (strategy_configure_native_v1(state.handle, &spec) == 0) {
        internal::count_settlement_paths(true);
        state.outcome.rc = strategy_native_run_v1(
            state.handle, reinterpret_cast<const pf_bar_t*>(tape.bars.data()),
            static_cast<int>(tape.bars.size()), nullptr);
        const auto counts = internal::settlement_path_counts();
        internal::count_settlement_paths(false);
        const int preview = static_cast<int>(internal::SettlementEntry::Preview);
        state.outcome.previews = counts.fused[preview] + counts.staged[preview];
    }

    auto* host = dynamic_cast<NativeStrategyHost*>(static_cast<BacktestEngine*>(state.handle));
    if (host != nullptr) {
        Outcome& out = state.outcome;
        out.completed = host->native_state().kind == NativeLifecycleKind::Completed;
        out.continuation = host->native_continuation_hash();
        out.broker = host->broker_state_hash();
        out.trades = host->trade_count();
        std::uint64_t digest = 1469598103934665603ull;
        for (int index = 0; index < out.trades; ++index) {
            const Trade& trade = host->get_trade(index);
            digest = k3_book::fnv_u64(digest, static_cast<std::uint64_t>(trade.entry_time));
            digest = k3_book::fnv_u64(digest, static_cast<std::uint64_t>(trade.exit_time));
            digest = k3_book::fnv_f64(digest, trade.entry_price);
            digest = k3_book::fnv_f64(digest, trade.exit_price);
            digest = k3_book::fnv_f64(digest, trade.qty);
            digest = k3_book::fnv_f64(digest, trade.pnl);
            digest = k3_book::fnv_f64(digest, trade.commission);
            digest = k3_book::fnv_f64(digest, trade.max_runup);
            digest = k3_book::fnv_f64(digest, trade.max_drawdown);
            digest = k3_book::fnv_u64(digest, trade.entry_incarnation);
        }
        out.trades_digest = digest;
        const auto events = host->native_events(0);
        out.events = events.size();
        std::uint64_t census = 1469598103934665603ull;
        for (const auto& event : events) {
            census = k3_book::fnv_u64(census, static_cast<std::uint64_t>(event.kind));
            census = k3_book::fnv_u64(census, event.ordinal);
            if (event.command) census = k3_book::fnv_u64(census, event.command->index());
        }
        out.events_digest = census;
        const auto& consumer = NativeExecutionConsumer::bound(*host);
        out.declared_bar_open = consumer.has_bar_open_hook();
    }
    strategy_native_host_free(state.handle);
    return state.outcome;
}

}  // namespace c_table
