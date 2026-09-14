#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cstdio>
#include <vector>
using namespace pineforge;
namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
Bar flat_bar(double p, int64_t ts) { return Bar{p, p, p, p, 1.0, ts}; }
class Probe final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 1) strategy_entry("L", true);
        if (bar_index_ == 4) strategy_close_all();
    }
};
// Review fix round 1, Important #2: stream_dispatch_script_bar
// (engine_stream.cpp) is a fourth script-bar dispatch site distinct from
// the three run() loops above. Same entry/close shape as Probe so both the
// warmup run() and the realtime-tick dispatch record.
class StreamProbe final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 1) strategy_entry("L", true);
        if (bar_index_ == 4) strategy_close_all();
    }
};
// Review fix round 1, Minor #3: exercise run_aggregation_bar_loop's
// non-magnifier branch (1m input -> 5m script), the one dispatch site the
// Probe/StreamProbe cases above don't reach.
class AggProbe final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
    }
};
}
int main() {
    std::vector<Bar> bars;
    for (int i = 0; i < 8; ++i) bars.push_back(flat_bar(100.0 + i, i * 60'000LL));
    Probe off; off.run(bars.data(), 8);
    ReportC r_off{}; off.fill_report(&r_off);
    CHECK(r_off.broker_state_hash_len == 0 && r_off.broker_state_hash == nullptr);
    BacktestEngine::free_report(&r_off);

    Probe on; on.set_broker_state_hash_recording(true); on.run(bars.data(), 8);
    ReportC r_on{}; on.fill_report(&r_on);
    CHECK(r_on.broker_state_hash_len == 8);
    CHECK(r_on.broker_state_hash[7] == on.broker_state_hash());          // array[last] == scalar
    CHECK(r_on.broker_state_hash[1] != r_on.broker_state_hash[2]);        // entry rested then filled
    // Prefix property on a deterministic engine: the run over bars[0..5]
    // records the same hashes as the first 6 of the run over bars[0..8].
    Probe pre; pre.set_broker_state_hash_recording(true); pre.run(bars.data(), 6);
    ReportC r_pre{}; pre.fill_report(&r_pre);
    for (int i = 0; i < 6; ++i) CHECK(r_pre.broker_state_hash[i] == r_on.broker_state_hash[i]);
    BacktestEngine::free_report(&r_pre);
    BacktestEngine::free_report(&r_on);

    // Review fix round 1, Important #2: the realtime stream path
    // (stream_begin's warmup run() + stream_dispatch_script_bar for every
    // tick-driven bar afterward) must satisfy the same
    // len == script_bars_processed / array[last] == broker_state_hash()
    // invariant pineforge.h promises for strategy_stream_fill_report.
    // Recording must be enabled BEFORE stream_begin to also cover the
    // warmup leg (reset_run_state, invoked by stream_begin's internal
    // run(), empties the recorded array but not the flag).
    {
        StreamProbe sp;
        sp.set_broker_state_hash_recording(true);
        std::vector<Bar> warmup;
        for (int i = 0; i < 5; ++i) warmup.push_back(flat_bar(100.0 + i, i * 60'000LL));
        CHECK(sp.stream_begin(warmup.data(), (int)warmup.size(), "1", "1"));
        CHECK(sp.last_error().empty());
        // Feed the remaining bars (indices 5..7) as one realtime tick each,
        // advancing the stream clock to the next input-bar boundary after
        // each push so every remaining bar completes and dispatches.
        for (int i = 5; i < 8; ++i) {
            const int64_t ts = i * 60'000LL;
            CHECK(sp.stream_push_tick(TradeTick{ts, static_cast<uint64_t>(i), 100.0 + i, 1.0}));
            CHECK(sp.stream_advance_time(ts + 60'000));
        }
        ReportC r_stream{};
        sp.fill_report(&r_stream);
        CHECK(r_stream.script_bars_processed == 8);
        CHECK(r_stream.broker_state_hash_len == r_stream.script_bars_processed);
        CHECK(r_stream.broker_state_hash[r_stream.broker_state_hash_len - 1] == sp.broker_state_hash());
        BacktestEngine::free_report(&r_stream);
        CHECK(sp.stream_end(false));
    }

    // Review fix round 1, Minor #3: an aggregated run (1m input -> 5m
    // script) dispatches through run_aggregation_bar_loop's non-magnifier
    // branch, which the single-TF Probe cases above never reach.
    {
        std::vector<Bar> agg_bars;
        for (int i = 0; i < 16; ++i) agg_bars.push_back(flat_bar(100.0 + i, i * 60'000LL));
        AggProbe ap;
        ap.set_broker_state_hash_recording(true);
        ap.run(agg_bars.data(), (int)agg_bars.size(), "1", "5");
        ReportC r_agg{};
        ap.fill_report(&r_agg);
        CHECK(r_agg.script_bars_processed > 0);
        CHECK(r_agg.broker_state_hash_len == r_agg.script_bars_processed);
        CHECK(r_agg.broker_state_hash[r_agg.broker_state_hash_len - 1] == ap.broker_state_hash());
        BacktestEngine::free_report(&r_agg);
    }

    return failures == 0 ? 0 : 1;
}
