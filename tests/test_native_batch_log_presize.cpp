// R5 lane PERF-K24 (K4). A batch sizes the logs it appends to for its own
// length instead of regrowing them by doubling.
//
// Every script bar of a run appends driver points to the consumer's driver
// log and, whenever the host commands, events to the command history. Both
// are append-only vectors, and until this lane a batch grew them one doubling
// at a time: each regrowth moved the whole log into a freshly faulted block
// and unmapped the old one, 24-37 % of a bare kernel's CPU in lane PERF0-K.
// NativeExecutionConsumer::reserve_driver_log existed, but only the source
// host called it, before a begin that reset the history it also reserved.
// Now pump_batch reserves the driver log exactly where its length is a fact of
// the spec (one script bar per input, a fixed number of points per script bar)
// and sizes every other log from the rate the batch has shown so far, never
// beyond a fixed factor of what the log already holds.
//
// Capacity is not observable through any kernel surface, so the witnesses
// count the global allocator's traffic during run():
//  - a batch whose driver log length is structural asks for it once, whole
//    (at 16,500 bars: four blocks of at least an eighth of the log before the
//    change, one after);
//  - what a batch still holds when it ends grows with the batch's length only
//    by the driver points it recorded: a longer run of the same host holds no
//    more history and no doubling slack (1.99 times the recorded points'
//    bytes between 16,500 and 66,000 bars before the change, 1.016 after).
// The value half pins every observable the pump feeds -- trades, driver
// points, command events, the continuation and the broker hash -- for the
// pump's paths: structural and projected driver logs, both label policies,
// an intrabar synthesized path with and without volume weighting, the
// after-calculation close, an aggregated script timeframe, and a stream's
// warmup. The spec's zone is a fixed offset, so the continuation folds no
// tzdata file and the pins hold on every host.
//
// Provenance of the pinned data: this TU compiled unchanged against the
// engine at fc7aad62 (before the change) with -DPINEFORGE_K24_HARVEST, which
// prints the observed values as the initializers below instead of checking
// them. Rebuild them the same way; never edit one by hand to make a run pass.
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <vector>

// Every allocation carries its size and whether it was made inside the
// recorded window in a header, so the window can count what it asked for and
// what of that it gave back. A block from before the window (a run's begin
// tears down what configure_native built) is never subtracted.
namespace {
struct Header {
    std::size_t size;
    std::size_t recorded;
};
constexpr std::size_t kHeader = 16;  // keeps the fundamental alignment
static_assert(sizeof(Header) <= kHeader, "allocation header");
constexpr std::size_t kMaxBlocks = std::size_t{1} << 16;
bool g_recording = false;
std::size_t g_live_bytes = 0;
std::size_t g_block_sizes[kMaxBlocks];  // what the window asked for, in order
std::size_t g_block_count = 0;          // may pass kMaxBlocks; sizes past it are dropped

void* counted_allocate(std::size_t n) {
    auto* raw = static_cast<unsigned char*>(std::malloc(n + kHeader));
    if (!raw) throw std::bad_alloc();
    const Header header{n, g_recording ? 1u : 0u};
    std::memcpy(raw, &header, sizeof header);
    if (g_recording) {
        g_live_bytes += n;
        if (g_block_count < kMaxBlocks) g_block_sizes[g_block_count] = n;
        ++g_block_count;
    }
    return raw + kHeader;
}

void counted_free(void* p) noexcept {
    if (!p) return;
    auto* raw = static_cast<unsigned char*>(p) - kHeader;
    Header header{};
    std::memcpy(&header, raw, sizeof header);
    if (g_recording && header.recorded) g_live_bytes -= header.size;
    std::free(raw);
}
}  // namespace

void* operator new(std::size_t n) { return counted_allocate(n); }
void* operator new[](std::size_t n) { return counted_allocate(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return counted_allocate(n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return counted_allocate(n);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { counted_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { counted_free(p); }

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int failures = 0;
#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr std::int64_t T = 1736121600000LL;  // 2025-01-06 00:00 UTC
constexpr std::int64_t kMinute = 60000;

std::int64_t wrap(std::int64_t value, std::int64_t period) {
    const std::int64_t r = value % period;
    return r < 0 ? r + period : r;
}

// A slow triangular trend with a one-point zig-zag, in quarter steps: every
// price sits on the 0.25 tick, and a bar swings through a bracket's legs.
double price_at(std::int64_t step) {
    const std::int64_t phase = wrap(step, 48);
    const std::int64_t triangle = phase < 24 ? phase : 48 - phase;
    return 100.0 + 0.25 * static_cast<double>(triangle) + (wrap(step, 2) == 0 ? 1.0 : -1.0);
}

std::vector<Bar> tape(int count, std::int64_t step_ms) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double open = price_at(i);
        const double close = price_at(i + 1);
        bars.push_back({open, (open > close ? open : close) + 0.25,
                        (open < close ? open : close) - 0.25, close,
                        1.0 + static_cast<double>(wrap(i, 4)),
                        T + static_cast<std::int64_t>(i) * step_ms});
    }
    return bars;
}

enum class Mode { Idle, Burst, Market, Reissue, Bracket };

no::Request leg(no::Trigger trigger, std::uint64_t group) {
    no::Request request;
    request.intent = no::Reduce{no::ExplicitUnits{1.0}};
    request.label = "x";
    request.trigger = trigger;
    no::Member member;
    member.group = group;
    member.cohort = 0;
    member.effect = no::GroupEffect::Cancel;
    request.group = member;
    return request;
}

// The shapes of PERF0-K's bare-kernel workloads: no orders; a burst of
// working orders on the first bar, cancelled on the second, then nothing; a
// market flip every ten bars; an entry whose two exit legs are re-priced
// every bar; a bracket armed by its entry's fill every sixth bar while flat.
struct Host final : NativeStrategyHost {
    Mode mode = Mode::Idle;
    int bar = 0;
    std::vector<no::RequestHandle> burst;
    std::optional<no::RequestHandle> take, stop;

    void on_native_run_begin() override {
        bar = 0;
        burst.clear();
        take.reset();
        stop.reset();
    }

    void place(std::optional<no::RequestHandle>& slot, const no::Request& request) {
        if (!slot) {
            const auto accepted = submit(request);
            if (accepted.handle) slot = *accepted.handle;
            return;
        }
        const auto replaced = replace(*slot, request);
        if (replaced.successor) slot = *replaced.successor; else slot.reset();
    }

    void on_native_bar(const Bar& b, const NativeDecisionContext&) override {
        ++bar;
        const double held = physical_position().signed_units;
        switch (mode) {
        case Mode::Idle:
            break;
        case Mode::Burst:
            if (bar == 1) {
                for (int i = 0; i < 100; ++i) {
                    no::Request request;
                    request.intent = no::Transact{1.0};
                    request.label = "b";
                    request.trigger = no::Limit{50.0 - 0.25 * i};
                    const auto accepted = submit(request);
                    if (accepted.handle) burst.push_back(*accepted.handle);
                }
            } else if (bar == 2) {
                for (const auto& handle : burst) cancel(handle);
            }
            break;
        case Mode::Market:
            if (bar % 10 == 0) {
                no::Request request;
                if (held == 0.0) request.intent = no::Transact{1.0}; else request.intent = no::Flatten{};
                request.label = "m";
                submit_market(request);
            }
            break;
        case Mode::Reissue:
            if (bar == 1) {
                no::Request request;
                request.intent = no::Transact{1.0};
                request.label = "L";
                submit_market(request);
            }
            if (held > 0.0) {
                place(take, leg(no::Limit{b.close + 40.0}, 1));
                place(stop, leg(no::Stop{b.close - 40.0}, 1));
            }
            break;
        case Mode::Bracket:
            if (held == 0.0 && bar % 6 == 0) {
                no::Request entry;
                entry.intent = no::Transact{1.0};
                entry.label = "E";
                const auto accepted = submit(entry);
                if (!accepted.handle) break;
                no::WaitForApplied owner;
                owner.parent = *accepted.handle;
                const auto group = static_cast<std::uint64_t>(bar);
                no::Request a = leg(no::Limit{b.close + 0.5}, group);
                no::Request s = leg(no::Stop{b.close - 0.5}, group);
                a.owner = owner;
                s.owner = owner;
                submit(a);
                submit(s);
            }
            break;
        }
    }
};

enum class Path { None, Synthesized, SynthesizedVolume };

struct Config {
    Mode mode;
    bool raw_labels;
    Path path;
    bool after_calculation;
    bool aggregated;  // input "1", script "5"
    bool stream;      // stream_begin over the tape but its last 40 bars, then push those
};

NativeRunSpec spec_for(const Config& c, const char* session_key) {
    NativeRunSpec spec;
    spec.identity = {session_key, 1};
    spec.input_tf = c.aggregated ? "1" : "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:K24";
    spec.timezone = "UTC+5";
    spec.session = "24x7";
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.25;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 1.0;
    if (c.raw_labels) spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    if (c.after_calculation) spec.close_execution = NativeCloseExecution::AfterCalculation;
    if (c.path != Path::None) {
        IntrabarPath::synthesized path;
        path.samples = 5;
        if (c.path == Path::SynthesizedVolume) {
            path.volume_weighted = true;
            path.volume_weighted_min_samples = 2;
            path.volume_weighted_max_samples = 9;
        }
        spec.intrabar.value = path;
    }
    return spec;
}

// ---- allocation witnesses ---------------------------------------------------

struct Traffic {
    std::vector<std::size_t> blocks;
    std::size_t held = 0;  // bytes the run allocated and had not freed at its end
    std::size_t driver_points = 0;
};

Traffic traffic(Mode mode, int count) {
    Host host;
    host.mode = mode;
    const Config config{mode, true, Path::None, false, false, false};
    const auto setup = host.configure_native(spec_for(config, "k24-traffic"));
    CHECK(setup.status == NativeSetupStatus::Applied);
    const std::vector<Bar> bars = tape(count, 5 * kMinute);
    Traffic out;
    g_block_count = 0;
    g_live_bytes = 0;
    g_recording = true;
    host.run(bars.data(), count);
    g_recording = false;
    out.held = g_live_bytes;
    CHECK(g_block_count <= kMaxBlocks);
    out.blocks.assign(g_block_sizes, g_block_sizes + std::min(g_block_count, kMaxBlocks));
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    for (const auto& event : host.native_events(0))
        if (event.driver) ++out.driver_points;
    return out;
}

constexpr int kTrafficBars = 16500;

void a_structural_batch_asks_for_its_driver_log_once() {
    const Traffic run = traffic(Mode::Idle, kTrafficBars);
    CHECK(run.driver_points == 4u * kTrafficBars);
    const std::size_t log_bytes = run.driver_points * sizeof(NativeDriverPoint);
    std::size_t large = 0, largest = 0;
    for (const std::size_t block : run.blocks) {
        if (block >= log_bytes / 8) ++large;
        if (block > largest) largest = block;
    }
    std::printf("idle batch of %d bars: %zu driver points, %zu blocks of at least an eighth "
                "of the log, largest %zu bytes for a %zu-byte log\n",
                kTrafficBars, run.driver_points, large, largest, log_bytes);
    CHECK(large == 1);
    CHECK(largest >= log_bytes);
}

void a_longer_batch_holds_only_the_points_it_recorded() {
    for (const Mode mode : {Mode::Idle, Mode::Burst}) {
        const Traffic small = traffic(mode, kTrafficBars);
        const Traffic large = traffic(mode, 4 * kTrafficBars);
        const std::size_t recorded =
            (large.driver_points - small.driver_points) * sizeof(NativeDriverPoint);
        const std::size_t grew = large.held - small.held;
        std::printf("%s batch, %d -> %d bars: holds %zu more bytes for %zu more bytes of "
                    "driver points (%.3fx)\n",
                    mode == Mode::Idle ? "idle" : "burst", kTrafficBars, 4 * kTrafficBars, grew,
                    recorded, static_cast<double>(grew) / static_cast<double>(recorded));
        CHECK(large.held >= small.held);
        CHECK(static_cast<double>(grew) <= 1.05 * static_cast<double>(recorded));
    }
}

// ---- the values the pump feeds ---------------------------------------------

struct Observed {
    int trades;
    std::uint64_t trade_digest;
    std::uint64_t driver_points;
    std::uint64_t command_events;
    std::uint64_t continuation;
    std::uint64_t broker;
};

std::uint64_t fnv(std::uint64_t h, double value) {
    unsigned char bytes[sizeof value];
    std::memcpy(bytes, &value, sizeof value);
    for (unsigned char byte : bytes) {
        h ^= byte;
        h *= 1099511628211ull;
    }
    return h;
}

Observed observe(const Config& c, int count, const char* session_key) {
    Host host;
    host.mode = c.mode;
    const auto setup = host.configure_native(spec_for(c, session_key));
    CHECK(setup.status == NativeSetupStatus::Applied);
    const std::vector<Bar> bars = tape(count, (c.aggregated ? 1 : 5) * kMinute);
    if (c.stream) {
        constexpr int kTail = 40;
        CHECK(host.stream_begin(bars.data(), count - kTail, "", ""));
        for (int i = count - kTail; i < count; ++i) CHECK(host.stream_push_bar(bars[i]));
        CHECK(host.stream_end());
    } else {
        host.run(bars.data(), count);
    }
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    Observed out{};
    out.trades = host.trade_count();
    out.trade_digest = 1469598103934665603ull;
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto& trade = host.get_trade(i);
        out.trade_digest = fnv(out.trade_digest, trade.entry_price);
        out.trade_digest = fnv(out.trade_digest, trade.exit_price);
        out.trade_digest = fnv(out.trade_digest, trade.qty);
        out.trade_digest = fnv(out.trade_digest, trade.pnl);
    }
    for (const auto& event : host.native_events(0)) {
        if (event.driver) ++out.driver_points;
        if (event.command) ++out.command_events;
    }
    out.continuation = host.native_continuation_hash();
    out.broker = host.broker_state_hash();
    return out;
}

struct Pinned {
    const char* name;
    Config config;
    int bars;
    Observed expected;
};

// Raw-label runs are long enough to reach the projection's later checkpoints;
// canonical ones stay short (their calendar is the costly part of a bar).
const Pinned kPinned[] = {
    {"k24-idle-raw", {Mode::Idle, true, Path::None, false, false, false}, 3000,
     {0, 0x14650fb0739d0383ull, 12000ull, 0ull, 15539970654880779486ull, 1122760425777655983ull}},
    {"k24-market-raw", {Mode::Market, true, Path::None, false, false, false}, 3000,
     {149, 0xffd16e3a42465223ull, 12000ull, 748ull, 2691280101907842234ull, 4988579385215033226ull}},
    {"k24-reissue-raw", {Mode::Reissue, true, Path::None, false, false, false}, 3000,
     {0, 0x14650fb0739d0383ull, 12000ull, 11996ull, 559917846442483075ull, 6145463019870918596ull}},
    {"k24-bracket-raw", {Mode::Bracket, true, Path::None, false, false, false}, 3000,
     {499, 0x48a8aa3386174115ull, 12000ull, 4494ull, 12662973670975696652ull, 13042963852800490529ull}},
    {"k24-burst-raw", {Mode::Burst, true, Path::None, false, false, false}, 3000,
     {0, 0x14650fb0739d0383ull, 12000ull, 200ull, 5169680609674392705ull, 15585026011916021389ull}},
    {"k24-bracket-synth", {Mode::Bracket, true, Path::Synthesized, false, false, false}, 3000,
     {499, 0xab07e4f5c19135baull, 15000ull, 4494ull, 645448626090986029ull, 513613226666104824ull}},
    {"k24-bracket-synth-volume",
     {Mode::Bracket, true, Path::SynthesizedVolume, false, false, false}, 3000,
     {499, 0xab07e4f5c19135baull, 15000ull, 4494ull, 6659251462066295108ull, 12441617458333709903ull}},
    {"k24-market-after-calc", {Mode::Market, true, Path::None, true, false, false}, 3000,
     {150, 0xe4072fd20cf282f3ull, 15000ull, 750ull, 16754399976542058856ull, 6653512602343788203ull}},
    {"k24-bracket-aggregated", {Mode::Bracket, true, Path::None, false, true, false}, 3000,
     {99, 0x4967ca06278a2d63ull, 2400ull, 845ull, 11576338549970754230ull, 2782850178320540896ull}},
    {"k24-reissue-canonical", {Mode::Reissue, false, Path::None, false, false, false}, 240,
     {0, 0x14650fb0739d0383ull, 960ull, 956ull, 3546404124219280572ull, 16042671105544878872ull}},
    {"k24-bracket-canonical", {Mode::Bracket, false, Path::None, false, false, false}, 240,
     {39, 0x5a55f2e6071c7686ull, 960ull, 354ull, 15954092093213330626ull, 15258347239628143751ull}},
    {"k24-bracket-stream", {Mode::Bracket, false, Path::None, false, false, true}, 240,
     {39, 0x5a55f2e6071c7686ull, 960ull, 354ull, 9764567397585213717ull, 16914546972851419556ull}},
};

void the_pump_feeds_the_same_run() {
    for (const auto& pin : kPinned) {
        const Observed got = observe(pin.config, pin.bars, pin.name);
#ifdef PINEFORGE_K24_HARVEST
        std::printf("     {%d, 0x%016" PRIx64 "ull, %" PRIu64 "ull, %" PRIu64 "ull, %" PRIu64
                    "ull, %" PRIu64 "ull}},  // %s\n",
                    got.trades, got.trade_digest, got.driver_points, got.command_events,
                    got.continuation, got.broker, pin.name);
#else
        const Observed& want = pin.expected;
        const bool same = got.trades == want.trades && got.trade_digest == want.trade_digest
            && got.driver_points == want.driver_points
            && got.command_events == want.command_events
            && got.continuation == want.continuation && got.broker == want.broker;
        if (!same) {
            std::fprintf(stderr,
                         "%s: trades %d digest %016" PRIx64 " points %" PRIu64 " events %" PRIu64
                         " continuation %" PRIu64 " broker %" PRIu64 "\n",
                         pin.name, got.trades, got.trade_digest, got.driver_points,
                         got.command_events, got.continuation, got.broker);
        }
        CHECK(same);
#endif
    }
}

} // namespace

int main() {
    the_pump_feeds_the_same_run();
#ifndef PINEFORGE_K24_HARVEST
    a_structural_batch_asks_for_its_driver_log_once();
    a_longer_batch_holds_only_the_points_it_recorded();
#endif
    if (failures == 0) std::printf("test_native_batch_log_presize: ok\n");
    return failures == 0 ? 0 : 1;
}
