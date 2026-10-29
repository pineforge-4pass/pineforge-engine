#pragma once

#include "live_report_patch.hpp"
#include <pineforge/engine.hpp>
#include <memory>

namespace pineforge {
inline namespace engine_script_run_v19 {

// A runner-owned view of one live stream, never broker or durable run state.
// Construct a new accumulator when beginning another stream. The first read
// is warmup (cursor zero); subsequent cursors are committed-message counts.
class LiveReportAccumulator {
public:
    explicit LiveReportAccumulator(std::uint64_t ratio_interval = 1);
    ~LiveReportAccumulator();
    LiveReportAccumulator(const LiveReportAccumulator&) = delete;
    LiveReportAccumulator& operator=(const LiveReportAccumulator&) = delete;

    report::LiveReportPatch update(const BacktestEngine& engine,
                                  std::uint64_t input_index, bool final_input = false);
    const report::LiveReportWork& work() const noexcept;
    std::uint64_t ratios_as_of_input() const noexcept;

private:
    friend class NativeExecutionConsumer;
    struct State;
    std::unique_ptr<State> state_;
};

}
}
