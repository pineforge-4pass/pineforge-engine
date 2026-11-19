#pragma once

#include "live_report_patch.hpp"
#include <pineforge/engine.hpp>
#include <pineforge/native_run_spec.hpp>
#include <memory>
#include <string>

namespace pineforge {
inline namespace engine_script_run_v20 {

// A runner-owned view of one live stream, never broker or durable run state.
// Construct a new accumulator when beginning another stream. The first read
// is warmup (cursor zero); subsequent cursors are committed-message counts.
class LiveReportAccumulator {
public:
    // The curve for the exact ratios stays resident: one point per report
    // point of the stream. For a short stream or a test.
    explicit LiveReportAccumulator(std::uint64_t ratio_interval = 1);
    // A long stream. The curve for the exact ratios lives in an anonymous
    // scratch file in `scratch_directory` (the engine's curve scratch writer;
    // an empty name selects its default disk directory) and the ratio routine
    // reads it back through one fixed window. When `released` names the
    // stream's own engine, the points this reader has taken also leave that
    // engine's vector, which then holds only the points since the previous
    // read; a read outside the ordinary live route puts them back first. The
    // patches and their metrics are the same in both modes. Throws when the
    // directory cannot back the scratch file: the engine's scratch refuses a
    // memory filesystem.
    LiveReportAccumulator(std::uint64_t ratio_interval, const std::string& scratch_directory,
                          BacktestEngine* released = nullptr);
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

// A native stream whose spec declares NativeEventRetention::Full, as the v1 C
// configuration does, keeps every driver point and account row for the life
// of the stream: a record that grows with each input. A caller that reads no
// event of its stream after the fact asks here to keep only `retention` of it
// from the stream's next begin on: Window (the unread window, flat in the
// input count), Commands (the whole command journal and every account row) or
// Full (the whole record, as 1.12 kept it, growing with each input). It is the
// kernel's readback switch (set_retention_override): the spec, its digest,
// every decision and every hash stay the spec's. What changes is what
// native_events() can still return, and native_events_dropped() counts every
// event a narrower retention let go unread. `engine` is a configured native
// host, not yet begun.
void keep_live_event_window(BacktestEngine& engine,
                            NativeEventRetention retention = NativeEventRetention::Window);

}
}
