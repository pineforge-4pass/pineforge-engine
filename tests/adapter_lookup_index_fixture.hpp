// R5 lane PERF-P7: randomized Pine command streams for the lookup-index
// differential rows.
//
// One seeded source host issues a stream of Pine commands every bar: market,
// limit and stop entries on a small pool of ids (reversals between cohorts,
// one id that opens on both sides, pyramiding adds), OCA groups on entries
// under both effects, strategy.exit brackets from_entry an id or every entry
// (dynamic, percent and explicit per-origin quantity legs, in their own
// groups, re-issued at moving levels, some issued while their parent is still
// flat), closes by id and by quantity, close_all, cancels by id, cancel_all
// and bracket cancels, raw orders -- over a tape with gaps and, for the
// leveraged configurations, slides deep enough to call margin, so brackets
// are suspended and revived around the margin calls. Every price is a whole
// number of quarter ticks and every choice comes from one xorshift stream,
// so a configuration replays bit for bit wherever it runs.
//
// A run is summarized as values: the native continuation read at every bar (it
// folds the kernel's command history, working book and account), the
// broker-state hash read every eighth bar (it folds the adapter's whole
// retained source state -- every placement row, cohort, bracket family and live
// handle), the final broker scalar and continuation, every trade and the error.
// The two differential rows (test_adapter_purge_index,
// test_adapter_exit_leg_index) run each configuration twice on fresh hosts --
// with the consumer's lookup indexes (the default) and with its reference
// switches off, which restores every scan the indexes replace -- and hold the
// two summaries equal bit for bit. The reversal family drives the cohort-side
// index (the applied-reversal purge, bracket_belongs_to_reversal) and the
// closed-row walk; the bracket family drives the consumed-leg index exit()
// reads.
#pragma once

#include "../src/native_execution_consumer.hpp"

#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace p7_stream {

using namespace pineforge;

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed)
        : state(seed * 0x9E3779B97F4A7C15ull ^ 0x2545F4914F6CDD1Dull) {
        if (state == 0) state = 1;
    }
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    int between(int lo, int hi) { return lo + below(hi - lo + 1); }
    bool percent(int p) { return below(100) < p; }
};

inline double ticks(long count) { return static_cast<double>(count) * 0.25; }

enum class Family { Reversals, Brackets };

struct StreamConfig {
    std::uint64_t seed = 1;
    Family family = Family::Reversals;
    int bars = 320;
    bool magnifier = false;
    bool margin = false;
    bool process_on_close = false;
    bool calc_on_fills = false;
    int pyramiding = 1;
};

inline const char* family_name(Family family) {
    return family == Family::Reversals ? "reversals" : "brackets";
}

// Quarter-tick bars: a random walk with occasional gaps and, when the
// configuration is leveraged, three slides of about a fifth of the price.
inline std::vector<Bar> make_tape(const StreamConfig& config) {
    Rng rng(config.seed ^ 0x7A3F11C5D2E90B47ull);
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(config.bars));
    long price = 400;
    const std::int64_t t0 = 1736121600000LL;
    for (int index = 0; index < config.bars; ++index) {
        long open = price;
        if (rng.percent(7)) open += rng.percent(50) ? rng.between(3, 12) : -rng.between(3, 12);
        long drift = rng.between(-5, 5);
        if (config.margin && ((index >= 50 && index < 62) || (index >= 140 && index < 150)
                              || (index >= 240 && index < 250)))
            drift = -rng.between(6, 9);
        long close = open + drift;
        if (open < 120) open = 120;
        if (close < 120) close = 120;
        const long high = (open > close ? open : close) + rng.between(0, 3);
        const long low = (open < close ? open : close) - rng.between(0, 3);
        Bar bar{};
        bar.open = ticks(open);
        bar.high = ticks(high);
        bar.low = ticks(low);
        bar.close = ticks(close);
        bar.volume = 1.0;
        bar.timestamp = t0 + static_cast<std::int64_t>(index) * 60000;
        bars.push_back(bar);
        price = close;
    }
    return bars;
}

// What a run produced, as values.
struct Outcome {
    std::vector<std::uint64_t> trace;
    std::uint64_t broker = 0;
    std::uint64_t continuation = 0;
    std::uint64_t trades_digest = 0;
    int trades = 0;
    std::string error;
    std::uint64_t answered = 0;
    std::uint64_t definitions_answered = 0;
    long commands = 0;
};

inline std::uint64_t fnv(std::uint64_t h, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
    return h;
}
inline std::uint64_t fnv_u64(std::uint64_t h, std::uint64_t v) { return fnv(h, &v, sizeof v); }
inline std::uint64_t fnv_f64(std::uint64_t h, double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return fnv_u64(h, bits);
}
inline std::uint64_t fnv_str(std::uint64_t h, const std::string& s) {
    h = fnv_u64(h, s.size());
    return fnv(h, s.data(), s.size());
}

class StreamHost final : public source::PineStrategyHost {
public:
    StreamHost(const StreamConfig& config, bool indexed) : config_(config), rng_(config.seed) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        source::PineStrategyConfig pine;
        pine.initial_capital = config.margin ? 20000.0 : 1000000.0;
        pine.default_qty_type = static_cast<int>(QtyType::FIXED);
        pine.default_qty_value = config.margin ? 700.0
            : (config.family == Family::Brackets ? 2.0 : 1.0);
        pine.pyramiding = config.pyramiding;
        pine.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        pine.commission_value = 0.25;
        pine.process_orders_on_close = config.process_on_close;
        pine.calc_on_order_fills = config.calc_on_fills;
        if (config.margin) {
            pine.margin_long = 25.0;
            pine.margin_short = 25.0;
        }
        configure_pine_strategy(pine);
        auto& consumer = as_native_consumer(execution_consumer());
        consumer.set_host_cache(indexed);
        consumer.set_definition_index(indexed);
    }

    long commands = 0;
    std::vector<std::uint64_t> trace;

    void on_source_bar(const Bar& bar) override {
        if (config_.family == Family::Reversals) reversals(bar);
        else brackets(bar);
        trace.push_back(native_continuation_hash());
        if (pine_bar_index() % 8 == 7) trace.push_back(broker_state_hash());
    }

    Outcome finish() {
        Outcome out;
        out.error = last_error();
        out.trace = trace;
        out.broker = broker_state_hash();
        out.continuation = native_continuation_hash();
        out.trades = trade_count();
        std::uint64_t digest = 1469598103934665603ull;
        for (int index = 0; index < out.trades; ++index) {
            const Trade& trade = get_trade(index);
            digest = fnv_u64(digest, static_cast<std::uint64_t>(trade.entry_time));
            digest = fnv_u64(digest, static_cast<std::uint64_t>(trade.exit_time));
            digest = fnv_f64(digest, trade.entry_price);
            digest = fnv_f64(digest, trade.exit_price);
            digest = fnv_f64(digest, trade.qty);
            digest = fnv_f64(digest, trade.pnl);
            digest = fnv_f64(digest, trade.commission);
            digest = fnv_u64(digest, trade.entry_incarnation);
            digest = fnv_str(digest, trade.entry_id);
            digest = fnv_str(digest, trade.exit_id);
        }
        out.trades_digest = digest;
        const auto& consumer = as_native_consumer(execution_consumer());
        if (const auto* cache = consumer.host_cache()) out.answered = cache->answered();
        out.definitions_answered = consumer.definition_index_answers();
        out.commands = commands;
        return out;
    }

private:
    static constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

    const char* long_id() { return rng_.percent(70) ? "L" : "L2"; }
    const char* short_id() { return rng_.percent(70) ? "S" : "S2"; }
    const char* any_id() {
        static const char* const ids[] = {"L", "L2", "S", "S2", "B"};
        return ids[rng_.below(5)];
    }
    const char* group() { return rng_.percent(50) ? "g1" : "g2"; }
    double near(const Bar& bar, int lo, int hi) {
        return bar.close + ticks(rng_.percent(50) ? rng_.between(lo, hi) : -rng_.between(lo, hi));
    }

    // Reversals between cohorts, an id that opens on both sides, priced
    // entries in OCA groups, brackets on the live side and brackets issued
    // while flat, closes and cancels.
    void reversals(const Bar& bar) {
        const double position = live_position_size();
        if (rng_.percent(35)) {
            ++commands;
            if (rng_.percent(50)) strategy_entry(long_id(), true);
            else strategy_entry(short_id(), false);
        }
        if (rng_.percent(10)) { ++commands; strategy_entry("B", rng_.percent(50)); }
        if (rng_.percent(12)) {
            ++commands;
            const bool buy = rng_.percent(50);
            const double level = near(bar, 1, 10);
            const bool limit = rng_.percent(50);
            const bool oca = rng_.percent(40);
            strategy_entry(buy ? long_id() : short_id(), buy, limit ? level : kNa,
                           limit ? kNa : level, kNa, {}, oca ? group() : "",
                           oca ? (rng_.percent(60) ? 1 : 2) : 0);
        }
        if (position != 0.0 && rng_.percent(45)) {
            ++commands;
            const bool is_long = position > 0.0;
            const char* from = is_long ? long_id() : short_id();
            const double width = ticks(rng_.between(4, 24));
            const double limit = is_long ? bar.close + width : bar.close - width;
            const double stop = is_long ? bar.close - width : bar.close + width;
            if (rng_.percent(60)) {
                strategy_exit(is_long ? "xl" : "xs", from, limit, stop);
            } else {
                strategy_exit(is_long ? "ql" : "qs", from, limit, stop, kNa, kNa, kNa, 100.0,
                              {}, 1.0, rng_.percent(50) ? "eg" : "");
            }
        }
        if (position == 0.0 && rng_.percent(12)) {
            ++commands;
            const bool buy = rng_.percent(50);
            const double width = ticks(rng_.between(4, 24));
            strategy_exit("xf", buy ? long_id() : short_id(), bar.close + width,
                          bar.close - width);
        }
        if (position != 0.0 && rng_.percent(8)) {
            ++commands;
            if (rng_.percent(50)) strategy_close(any_id(), {}, 1.0);
            else strategy_close(any_id(), {}, kNa, 50.0, rng_.percent(30));
        }
        if (rng_.percent(3)) { ++commands; strategy_close_all(); }
        if (rng_.percent(5)) { ++commands; strategy_cancel(any_id()); }
        if (rng_.percent(2)) { ++commands; strategy_cancel_all(); }
        if (rng_.percent(6)) {
            ++commands;
            strategy_order("o", rng_.percent(50), 1.0, rng_.percent(50) ? near(bar, 1, 6) : kNa);
        }
        if (rng_.percent(3)) { ++commands; strategy_exit_cancel_bracket("xl", "L"); }
    }

    // Per-origin quantity brackets around the fill price, re-issued at moving
    // levels (the oca-multi-bracket shape), a percent bracket, legs issued
    // while the parent is flat, pyramiding adds, bracket cancels, closes and
    // an occasional reversal.
    void brackets(const Bar& bar) {
        const double position = live_position_size();
        if (position == 0.0 && rng_.percent(15)) {
            ++commands;
            strategy_exit("X_P", "L", bar.close + 2.0, bar.close - 2.0, kNa, kNa, kNa, 100.0,
                          {}, 1.0, "GRP_P");
        }
        if (position == 0.0 && rng_.percent(30)) {
            ++commands;
            if (rng_.percent(80)) strategy_entry("L", true, kNa, kNa, rng_.percent(50) ? 2.0 : 3.0);
            else strategy_entry("S", false, kNa, kNa, 2.0);
        }
        if (position > 0.0 && config_.pyramiding > 1 && rng_.percent(8)) {
            ++commands;
            strategy_entry(rng_.percent(50) ? "L" : "L2", true, kNa, kNa, 1.0);
        }
        if (position != 0.0) {
            const bool is_long = position > 0.0;
            const char* from = is_long ? "L" : "S";
            const double entry = position_avg_price();
            const double sign = is_long ? 1.0 : -1.0;
            const double tight = ticks(rng_.between(2, 6));
            const double wide = ticks(rng_.between(12, 24));
            if (rng_.percent(85)) {
                ++commands;
                strategy_exit("X_A", from, entry + sign * tight, entry - sign * tight, kNa, kNa,
                              kNa, 100.0, {}, 1.0, "GRP_A");
            }
            if (rng_.percent(85)) {
                ++commands;
                strategy_exit("X_B", from, entry + sign * wide, entry - sign * wide, kNa, kNa,
                              kNa, 100.0, {}, 1.0, "GRP_B");
            }
            if (rng_.percent(20)) {
                ++commands;
                strategy_exit("X_C", rng_.percent(50) ? from : "", entry + sign * wide,
                              entry - sign * tight, kNa, kNa, kNa, 50.0);
            }
            if (rng_.percent(4)) { ++commands; strategy_exit_cancel_bracket("X_B", from); }
            if (rng_.percent(3)) { ++commands; strategy_close(from, {}, 1.0); }
            if (rng_.percent(2)) { ++commands; strategy_close_all(); }
            if (rng_.percent(3)) {
                ++commands;
                strategy_entry(is_long ? "S" : "L", !is_long, kNa, kNa, 2.0);
            }
        }
        if (rng_.percent(2)) { ++commands; strategy_cancel(rng_.percent(50) ? "L" : "S"); }
    }

    StreamConfig config_;
    Rng rng_;
};

inline Outcome run_stream(const StreamConfig& config, bool indexed) {
    StreamHost host(config, indexed);
    const std::vector<Bar> bars = make_tape(config);
    if (config.magnifier) {
        host.run(bars.data(), static_cast<int>(bars.size()), "", "", true, 4,
                 MagnifierDistribution::ENDPOINTS);
    } else {
        host.run(bars.data(), static_cast<int>(bars.size()));
    }
    return host.finish();
}

// Every configuration a differential row runs: seeds under each run shape --
// chart and magnifier paths, flat and leveraged accounts, one and two
// pyramiding slots, and the process-on-close and fill-recalculation cadences.
inline std::vector<StreamConfig> configurations(Family family, int seeds) {
    std::vector<StreamConfig> out;
    for (int seed = 1; seed <= seeds; ++seed) {
        StreamConfig config;
        config.seed = static_cast<std::uint64_t>(seed) * 7919u + (family == Family::Brackets ? 1 : 0);
        config.family = family;
        config.magnifier = seed % 2 == 0;
        config.margin = seed % 3 == 0;
        config.pyramiding = seed % 4 < 2 ? 1 : 2;
        config.process_on_close = seed % 5 == 1;
        config.calc_on_fills = seed % 7 == 3 && !config.magnifier;
        out.push_back(config);
    }
    return out;
}

// The first value two runs of one configuration disagree on, or empty.
inline std::string first_difference(const Outcome& indexed, const Outcome& scanned) {
    if (indexed.error != scanned.error) return "error";
    if (indexed.trace.size() != scanned.trace.size()) return "trace length";
    for (std::size_t i = 0; i < indexed.trace.size(); ++i)
        if (indexed.trace[i] != scanned.trace[i]) return "trace value " + std::to_string(i);
    if (indexed.broker != scanned.broker) return "broker hash";
    if (indexed.continuation != scanned.continuation) return "continuation hash";
    if (indexed.trades != scanned.trades) return "trade count";
    if (indexed.trades_digest != scanned.trades_digest) return "trades";
    if (indexed.commands != scanned.commands) return "command stream";
    return {};
}

}  // namespace p7_stream
