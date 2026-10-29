// Compile against either header closure: replay must preserve the engine epoch.
#include <pineforge/engine.hpp>

#include <cstddef>
#include <cstdio>
#include <string>

namespace {
using Engine = pineforge::BacktestEngine;
constexpr std::size_t text_bytes = sizeof(std::string);
constexpr std::size_t sink_bytes = 360 + 3 * text_bytes;
static_assert(sizeof(void*) == 8 && sizeof(int) == 4);
static_assert(text_bytes == 24 || text_bytes == 32);
static_assert(sizeof(pineforge::ReportMode) == 4);
static_assert(alignof(Engine) == 8);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
template<class State>
struct SinkLayout {
    static_assert(sizeof(State) == sink_bytes, "report state changed the engine epoch");
    static_assert(alignof(State) == 8);
    static_assert(offsetof(State, enabled) == 0);
    static_assert(offsetof(State, trade_history_streamable) == 1);
    static_assert(offsetof(State, path) == 8);
    static_assert(offsetof(State, run_id) == 8 + text_bytes);
    static_assert(offsetof(State, scratch_dir) == 8 + 2 * text_bytes);
    static_assert(offsetof(State, fd) == 8 + 3 * text_bytes);
    static_assert(offsetof(State, fd_device) == 16 + 3 * text_bytes);
    static_assert(offsetof(State, fd_inode) == 24 + 3 * text_bytes);
    static_assert(offsetof(State, fd_identity_valid) == 32 + 3 * text_bytes);
    static_assert(offsetof(State, active_calls) == 36 + 3 * text_bytes);
    static_assert(offsetof(State, stream_calls) == 40 + 3 * text_bytes);
    static_assert(offsetof(State, writer) == 48 + 3 * text_bytes);
    static_assert(offsetof(State, equity_scratch) == 56 + 3 * text_bytes);
    static_assert(offsetof(State, equity_tail_stored) == 64 + 3 * text_bytes);
    static_assert(offsetof(State, receipt) == 72 + 3 * text_bytes);
    static_assert(offsetof(State, summary) == 216 + 3 * text_bytes);
    static_assert(offsetof(State, equity_prefix) == 280 + 3 * text_bytes);
    static_assert(offsetof(State, output_base) == 312 + 3 * text_bytes);
    static_assert(offsetof(State, selected_equity_stream) == 320 + 3 * text_bytes);
    static_assert(offsetof(State, selected_released_extremes) == 328 + 3 * text_bytes);
};
// As with the existing private-member test seam, explicit instantiation may
// name private members. No access macro or production friendship is needed.
template struct SinkLayout<Engine::ReportSinkState>;

struct LayoutTag {};
void describe_layout(LayoutTag);
template<std::size_t SinkOffset, std::size_t ModeOffset>
struct EngineLayout {
    static_assert(ModeOffset == SinkOffset + sink_bytes,
                  "the capture policy moved relative to the report state");
    friend void describe_layout(LayoutTag) {
        std::printf("report state=%zu, state offset=%zu, capture offset=%zu\n",
                    sink_bytes, SinkOffset, ModeOffset);
    }
};
template struct EngineLayout<offsetof(Engine, report_sink_state_),
                             offsetof(Engine, report_mode_)>;
#pragma GCC diagnostic pop
}

int main() {
    describe_layout(LayoutTag{});
}
