// R5 lane D2-A: the value comparisons its run-level witnesses share.
//
// Each witness runs one configuration twice -- with a short-cut and without
// it, or on a host that declared a hook absent and on one that implements it
// empty -- and every value the two runs produced must be equal. The runs come
// from K3's randomized books (native_match_book_fixture.hpp) and PERF-L2's
// hosts (native_fused_settlement_fixture.hpp); this header compares their
// outcomes field by field, and adds what K3's outcome does not keep: every
// closed row's and open lot's excursions, which the kernel's per-point
// excursion walk writes.
//
// Source-free: kernel-only builds register the rows that use it.
#pragma once

#include "native_fused_settlement_fixture.hpp"
#include "native_match_book_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace l2_fused {
// Found by argument-dependent lookup from first_divergence below.
inline bool operator==(const FillRecord& a, const FillRecord& b) {
    return a.ordinal == b.ordinal && a.digest == b.digest && a.continuation == b.continuation
        && a.broker == b.broker;
}
}  // namespace l2_fused

namespace outcome_compare {

using namespace pineforge;

// A witness's running tally: every comparison counts, every mismatch fails.
struct Tally {
    int failures = 0;
    long checks = 0;
    void check(bool condition, const char* what, const char* file, int line) {
        ++checks;
        if (!condition) {
            std::fprintf(stderr, "FAIL %s:%d %s\n", file, line, what);
            ++failures;
        }
    }
};

#define OUTCOME_CHECK(tally, condition) (tally).check((condition), #condition, __FILE__, __LINE__)

inline std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

// Every closed row's excursions and every open lot's, at `mark`.
inline std::uint64_t excursion_digest(const NativeStrategyHost& host, double mark) {
    std::uint64_t h = 1469598103934665603ull;
    for (int index = 0; index < host.trade_count(); ++index) {
        const Trade& trade = host.get_trade(index);
        h = k3_book::fnv_f64(h, trade.max_runup);
        h = k3_book::fnv_f64(h, trade.max_drawdown);
    }
    for (const auto& lot : host.native_open_lots(mark)) {
        h = k3_book::fnv_u64(h, lot.entry_incarnation);
        h = k3_book::fnv_f64(h, lot.favorable_excursion);
        h = k3_book::fnv_f64(h, lot.adverse_excursion);
    }
    return h;
}

template <class T>
void first_divergence(Tally& tally, const std::vector<T>& a, const std::vector<T>& b,
                      const char* what) {
    OUTCOME_CHECK(tally, a.size() == b.size());
    const std::size_t common = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t index = 0; index < common; ++index) {
        if (!(a[index] == b[index])) {
            std::fprintf(stderr, "  first %s divergence at %zu of %zu\n", what, index, common);
            ++tally.failures;
            return;
        }
    }
}

inline bool same(Tally& tally, const k3_book::Outcome& a, const k3_book::Outcome& b) {
    const int before = tally.failures;
    OUTCOME_CHECK(tally, a.completed);
    OUTCOME_CHECK(tally, b.completed);
    OUTCOME_CHECK(tally, a.error == b.error);
    first_divergence(tally, a.trace, b.trace, "continuation");
    OUTCOME_CHECK(tally, a.continuation == b.continuation);
    OUTCOME_CHECK(tally, a.broker == b.broker);
    OUTCOME_CHECK(tally, a.trades == b.trades);
    OUTCOME_CHECK(tally, a.trades_digest == b.trades_digest);
    OUTCOME_CHECK(tally, a.events == b.events);
    OUTCOME_CHECK(tally, a.events_digest == b.events_digest);
    OUTCOME_CHECK(tally, bits(a.position) == bits(b.position));
    OUTCOME_CHECK(tally, a.accepted == b.accepted);
    OUTCOME_CHECK(tally, a.rejected == b.rejected);
    OUTCOME_CHECK(tally, a.replaced == b.replaced);
    OUTCOME_CHECK(tally, a.cancelled == b.cancelled);
    OUTCOME_CHECK(tally, a.applied == b.applied);
    OUTCOME_CHECK(tally, a.unordered == 0);
    OUTCOME_CHECK(tally, b.unordered == 0);
    return tally.failures == before;
}

inline bool same(Tally& tally, const l2_fused::Outcome& a, const l2_fused::Outcome& b) {
    const int before = tally.failures;
    OUTCOME_CHECK(tally, a.completed == b.completed);
    OUTCOME_CHECK(tally, a.error == b.error);
    first_divergence(tally, a.trace, b.trace, "continuation");
    first_divergence(tally, a.fills, b.fills, "fill");
    OUTCOME_CHECK(tally, a.precommits == b.precommits);
    OUTCOME_CHECK(tally, a.excursions == b.excursions);
    OUTCOME_CHECK(tally, a.continuation == b.continuation);
    OUTCOME_CHECK(tally, a.broker == b.broker);
    OUTCOME_CHECK(tally, a.stream_hash == b.stream_hash);
    OUTCOME_CHECK(tally, a.stream_actions == b.stream_actions);
    OUTCOME_CHECK(tally, a.trades == b.trades);
    OUTCOME_CHECK(tally, a.trades_digest == b.trades_digest);
    OUTCOME_CHECK(tally, a.lots_digest == b.lots_digest);
    OUTCOME_CHECK(tally, a.events == b.events);
    OUTCOME_CHECK(tally, a.events_digest == b.events_digest);
    OUTCOME_CHECK(tally, bits(a.position) == bits(b.position));
    OUTCOME_CHECK(tally, bits(a.equity) == bits(b.equity));
    OUTCOME_CHECK(tally, a.accepted == b.accepted);
    OUTCOME_CHECK(tally, a.rejected == b.rejected);
    OUTCOME_CHECK(tally, a.replaced == b.replaced);
    OUTCOME_CHECK(tally, a.cancelled == b.cancelled);
    OUTCOME_CHECK(tally, a.applied == b.applied);
    OUTCOME_CHECK(tally, a.refusals == b.refusals);
    return tally.failures == before;
}

inline void describe(const k3_book::BookConfig& config, const char* extra) {
    std::fprintf(stderr, "  k3 book seed=%llu live=%d bars=%d path=%s calc_on_fills=%d "
                 "quantize=%d %s\n",
                 static_cast<unsigned long long>(config.seed), config.live, config.bars,
                 k3_book::path_name(config.path), config.calc_on_fills ? 1 : 0,
                 config.quantize ? 1 : 0, extra);
}

inline void describe(const l2_fused::Config& config, const char* extra) {
    std::fprintf(stderr, "  l2 host seed=%llu path=%s fee=%s fx=%s margin=%d excursions=%d "
                 "stream=%d calc_on_fills=%d %s\n",
                 static_cast<unsigned long long>(config.seed), l2_fused::path_name(config.path),
                 l2_fused::fee_name(config.fee_kind), l2_fused::fx_name(config.fx),
                 config.margin ? 1 : 0, config.owns_excursions ? 1 : 0,
                 config.drive == l2_fused::Drive::Stream ? 1 : 0, config.calc_on_fills ? 1 : 0,
                 extra);
}

// K3's and PERF-L2's specs label their inputs FeedTolerant, under which a
// retained lower feed's windows are zero-width, so every script bar of their
// lower-timeframe runs falls back to its own OHLC path
// (tests/test_native_intrabar_lower_lookup.cpp). These witnesses label a
// lower-timeframe run Canonical instead, so its sub-bars are walked.
inline void walk_lower_bars(NativeRunSpec& spec) {
    if (spec.intrabar.lower()) spec.slot_label_policy = NativeSlotLabelPolicy::Canonical;
}

// k3_book::run_book with the spec adjusted before it is configured.
template <class Host, class Adjust>
k3_book::Outcome run_book(Host& host, const k3_book::BookConfig& config, Adjust adjust) {
    const k3_book::Tape tape = k3_book::make_tape(config);
    NativeRunSpec spec = k3_book::make_spec(config, tape);
    walk_lower_bars(spec);
    adjust(spec);
    if (host.configure_native(spec).status != NativeSetupStatus::Applied) {
        host.outcome.error = "configure_native refused the spec";
        return host.outcome;
    }
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    host.finish();
    return host.outcome;
}

// l2_fused::run, the same steps, with the spec adjusted before it is
// configured.
template <class Host, class Adjust>
l2_fused::Outcome run_fused(Host& host, const l2_fused::Config& config, Adjust adjust) {
    const l2_fused::Tape tape = l2_fused::make_tape(config);
    host.last_close_ = tape.bars.back().close;
    NativeRunSpec spec = l2_fused::make_spec(config, tape);
    walk_lower_bars(spec);
    adjust(spec);
    if (host.configure_native(spec).status != NativeSetupStatus::Applied) {
        host.outcome.error = "configure_native refused the spec";
        return host.outcome;
    }
    if (config.fx == l2_fused::Fx::Curve) {
        const NativeFxCurve curve{{l2_fused::kT0 + 150 * l2_fused::kMinute,
                                   l2_fused::kT0 + 400 * l2_fused::kMinute},
                                  {1.25, 0.8}};
        if (host.configure_native_fx_curve(curve).status != NativeSetupStatus::Applied) {
            host.outcome.error = "configure_native_fx_curve refused the curve";
            return host.outcome;
        }
    }
    if (config.drive == l2_fused::Drive::Stream) {
        const int warmup = static_cast<int>(tape.bars.size()) / 3;
        bool ok = host.stream_begin(tape.bars.data(), warmup, "5", "5");
        for (std::size_t i = static_cast<std::size_t>(warmup); ok && i < tape.bars.size(); ++i)
            ok = host.stream_push_bar(tape.bars[i]);
        if (ok) host.stream_end();
    } else {
        host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    }
    host.finish();
    return host.outcome;
}

inline void as_is(NativeRunSpec&) {}

// The L2 configurations every witness sweeps: each intrabar path under each
// fee form, account FX (a stepping curve with the margin model among them),
// host-owned excursions and stream driving.
inline std::vector<l2_fused::Config> fused_configs(std::uint64_t first_seed) {
    std::vector<l2_fused::Config> configs;
    std::uint64_t seed = first_seed;
    for (const l2_fused::Path path :
         {l2_fused::Path::None, l2_fused::Path::Synthesized, l2_fused::Path::Lower}) {
        for (int variant = 0; variant < 8; ++variant) {
            l2_fused::Config config;
            config.seed = seed++;
            config.bars = 90;
            config.path = path;
            config.fee_kind = variant % 3 == 0 ? NativeFeeKind::Percent
                : (variant % 3 == 1 ? NativeFeeKind::CashPerUnit : NativeFeeKind::CashPerExecution);
            config.fee_value = config.fee_kind == NativeFeeKind::Percent ? 0.05 : 0.5;
            config.fx = variant == 2 || variant == 5 ? l2_fused::Fx::Curve
                : (variant == 4 ? l2_fused::Fx::Constant : l2_fused::Fx::Unit);
            config.margin = variant == 3 || variant == 5 || variant == 6;
            config.flatten_liquidations = variant == 6;
            config.owns_excursions = variant == 1 || variant == 7;
            config.calc_on_fills = variant == 4 || variant == 7;
            config.drive = variant == 7 ? l2_fused::Drive::Stream : l2_fused::Drive::Batch;
            configs.push_back(config);
        }
    }
    return configs;
}

}  // namespace outcome_compare
