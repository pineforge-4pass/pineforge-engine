// core-cr150-o1e3b row E17: the batch realtime-tick transport.
//
// The C entry point used to reserve and fill one TradeTick vector per call, so
// a batch cost heap bytes proportional to its length before a single row was
// delivered. It now hands the consumer the caller's C array and a reader that
// decodes one borrowed POD at a time (native_stream_push_ticks_borrowed).
//
// The two properties proven here are independent of that implementation:
//
//   1. Batch and per-tick delivery answer the same run. The C contract is
//      "semantically identical to repeated strategy_stream_push_tick", so the
//      whole-run broker-state fold of the same warmup and the same 4096 ticks
//      must be equal through both routes.
//   2. Batch delivery retains no batch-sized copy. The probe measures the
//      bytes still live across the single push call; the old copy alone is
//      kBatch * sizeof(TradeTick) = 131072 bytes, so the bound below is a real
//      RED at the base commit and not a formality.
//
// Both assertions are read from executed results: no expected hash is
// hard-coded, and the peak bound is compared against a measured allocation,
// not a container estimate.
#include "o1e3_allocation_probe.hpp"

// load-bearing: pineforge.h before engine.hpp keeps the per-strategy
// declarations visible (engine.hpp defines PINEFORGE_NO_STRATEGY_DECLS).
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {

void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

// The smallest host the streaming lifecycle accepts: the stream is driven
// entirely through the C entry points and on_source_bar has no work to do.
class TickEngine final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const pineforge::Bar&) override {}
};

constexpr std::size_t kBatch = 4096;
// 131072 bytes of TradeTick copies at the base commit; the delivered work
// itself keeps no batch-sized buffer.
constexpr std::size_t kWorkspaceBound = 65536;

pf_trade_tick_t tick_at(std::size_t index) {
    pf_trade_tick_t tick{};
    tick.timestamp = 60010 + static_cast<int64_t>(index);
    tick.sequence = static_cast<std::uint64_t>(index) + 1;
    tick.price = 100.0 + static_cast<double>(index % 4) * 0.25;
    tick.quantity = 1.0;
    return tick;
}

// One complete stream over the same warmup and ticks, either one call per
// tick or one batch call. Returns the run's broker-state fold and, for the
// batch route, the live-allocation peak of that single push call.
std::uint64_t run_stream(const std::vector<pf_trade_tick_t>& ticks, bool batch,
                         std::size_t* peak) {
    TickEngine engine;
    pf_strategy_t handle = static_cast<pf_strategy_t>(&engine);
    pf_bar_t warmup{};
    warmup.open = warmup.high = warmup.low = warmup.close = 100.0;
    warmup.volume = 2.0;
    warmup.timestamp = 0;
    require(strategy_stream_begin(handle, &warmup, 1, "1", "1") == 0,
            "E17 fixture stream_begin refused");
    if (batch) {
        o1e3_test::AllocationProbe memory;
        require(strategy_stream_push_ticks(handle, ticks.data(),
                                           static_cast<int>(ticks.size())) == 0,
                "E17 batch push refused");
        *peak = memory.stop();
    } else {
        for (const auto& tick : ticks) {
            require(strategy_stream_push_tick(handle, &tick) == 0, "E17 single push refused");
        }
    }
    require(strategy_stream_advance_time(handle, 120000) == 0, "E17 advance time refused");
    require(strategy_stream_end(handle, 0) == 0, "E17 stream end refused");
    return engine.broker_state_hash();
}

} // namespace

int main() {
    try {
        std::vector<pf_trade_tick_t> ticks;
        ticks.reserve(kBatch);
        for (std::size_t i = 0; i < kBatch; ++i) ticks.push_back(tick_at(i));

        std::size_t peak = 0;
        const auto single = run_stream(ticks, false, &peak);
        const auto batch = run_stream(ticks, true, &peak);
        require(batch == single, "E17 batch stream differs from the per-tick stream");
        require(peak <= kWorkspaceBound,
                "E17 batch push retained a population-sized tick copy");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}
