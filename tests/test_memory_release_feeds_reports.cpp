// Feed and report scratch storage is checked at call and owner boundaries;
// the recut check proves nothing the path allocated outlives its host.
// Equality expectations come from ownership/lifetime, never a measured byte
// allowance. The allocator charges ALL new/delete forms across all threads.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_scheduler.hpp>
#include <pineforge/pineforge.h>
#include "../src/broker_state_hash_internal.hpp"
#include "../src/report_replay.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// These two runner implementation units are not part of libpineforge. Include
// their unmodified source so the test registration links the real bodies
// without pulling in the optional runner's SQLite/curl dependencies. Neither
// State's fields nor an alternate implementation is used as a test observer.
#include "../src/live_report.cpp"
#include "../runner/ratio_report.cpp"

namespace feed_report_heap {
// Every allocation, including allocations on other threads, is charged.
// Only main enables this flag; samples verify that they run on that thread.
// No allocation is filtered by a measurement window or by its thread.
// Atomics make a release from another thread safe; no counter is ever reset.
thread_local bool test_thread = false;
std::atomic<std::size_t> live_bytes{0}, live_blocks{0}, allocations{0};
struct Header { std::size_t bytes, prefix; bool charged; };
constexpr std::size_t fundamental = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
constexpr std::size_t header_bytes =
    (sizeof(Header) + fundamental - 1) / fundamental * fundamental;

void* allocate(std::size_t bytes, std::size_t alignment) noexcept {
    if (!bytes) bytes = 1;
    alignment = std::max(alignment, fundamental);
    const auto prefix = std::max(header_bytes, alignment);
    if (bytes > std::numeric_limits<std::size_t>::max() - prefix) return nullptr;
    void* base = nullptr;
    if (alignment <= fundamental) base = std::malloc(prefix + bytes);
    else if (::posix_memalign(&base, alignment, prefix + bytes) != 0) return nullptr;
    if (!base) return nullptr;
    auto* result = static_cast<unsigned char*>(base) + prefix;
    const Header header{bytes, prefix, true};
    std::memcpy(result - sizeof header, &header, sizeof header);
    if (header.charged) {
        live_bytes.fetch_add(bytes, std::memory_order_relaxed);
        live_blocks.fetch_add(1, std::memory_order_relaxed);
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}
void release(void* pointer) noexcept {
    if (!pointer) return;
    auto* result = static_cast<unsigned char*>(pointer);
    Header header{};
    std::memcpy(&header, result - sizeof header, sizeof header);
    if (header.charged) {
        live_bytes.fetch_sub(header.bytes, std::memory_order_relaxed);
        live_blocks.fetch_sub(1, std::memory_order_relaxed);
    }
    std::free(result - header.prefix);
}
void* throwing(std::size_t bytes, std::size_t alignment = fundamental) {
    for (;;) {
        if (void* result = allocate(bytes, alignment)) return result;
        const auto handler = std::get_new_handler();
        if (!handler) throw std::bad_alloc();
        handler();
    }
}
void* nothrow(std::size_t bytes, std::size_t alignment = fundamental) noexcept {
    try { return throwing(bytes, alignment); }
    catch (...) { return nullptr; }
}
struct Sample { std::size_t bytes, blocks, calls; bool in_test_thread; };
Sample sample() noexcept {
    return {live_bytes.load(std::memory_order_relaxed),
            live_blocks.load(std::memory_order_relaxed),
            allocations.load(std::memory_order_relaxed), test_thread};
}
bool same_live(Sample a, Sample b) noexcept {
    return a.in_test_thread && b.in_test_thread && a.bytes == b.bytes && a.blocks == b.blocks;
}
} // namespace feed_report_heap

void* operator new(std::size_t n) { return feed_report_heap::throwing(n); }
void* operator new[](std::size_t n) { return feed_report_heap::throwing(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    return feed_report_heap::throwing(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
    return feed_report_heap::throwing(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return feed_report_heap::nothrow(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return feed_report_heap::nothrow(n);
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return feed_report_heap::nothrow(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return feed_report_heap::nothrow(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { feed_report_heap::release(p); }
void operator delete[](void* p) noexcept { feed_report_heap::release(p); }
void operator delete(void* p, std::size_t) noexcept { feed_report_heap::release(p); }
void operator delete[](void* p, std::size_t) noexcept { feed_report_heap::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { feed_report_heap::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { feed_report_heap::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { feed_report_heap::release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { feed_report_heap::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { feed_report_heap::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { feed_report_heap::release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    feed_report_heap::release(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    feed_report_heap::release(p);
}

namespace {
using namespace pineforge;
using SourceHost = source::PineStrategyHost;
using Adapter = source::PineExecutionAdapter;
namespace no = native_order;
namespace heap = feed_report_heap;
constexpr std::int64_t start = INT64_C(1736121600000);
constexpr std::int64_t minute = 60000;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();
std::size_t checks = 0, failures = 0;

bool check(const char* row, bool value, int line, const char* expression,
           const char* explanation) {
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s -- %s\n",
                     row, __FILE__, line, expression, explanation);
    }
    return value;
}
#define ROW(row, expression, explanation) \
    check(row, static_cast<bool>(expression), __LINE__, #expression, explanation)

// Repository-standard explicit instantiation. No product friend/accessor,
// copied class definition, layout cast, or insertion into a target local.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct PineStates { friend auto access(PineStates); };
struct FeedPine { friend auto access(FeedPine); };
struct FeedAux { friend auto access(FeedAux); };
struct SchedulerMember { friend auto access(SchedulerMember); };
struct FrozenEntries { friend auto access(FrozenEntries); };
struct ScheduleLagged { friend auto access(ScheduleLagged); };
struct CloseScope { friend auto access(CloseScope); };
struct LaggedScope { friend auto access(LaggedScope); };
struct FollowScope { friend auto access(FollowScope); };
struct PolicyBar { friend auto access(PolicyBar); };
struct PolicyValid { friend auto access(PolicyValid); };
template struct Access<PineStates, &SourceHost::pine_security_states_>;
template struct Access<FeedPine, &SourceHost::pine_feed_security_eval_state>;
template struct Access<FeedAux, &SourceHost::feed_aux_security_for_chart_bar>;
template struct Access<SchedulerMember, &SourceHost::scheduler_>;
template struct Access<FrozenEntries, &source::PineLanguageState::pos_view_frozen_entry_qty_>;
template struct Access<ScheduleLagged, &Adapter::schedule_lagged_short_follow_up>;
template struct Access<CloseScope, &Adapter::close_point_margin_scope>;
template struct Access<LaggedScope, &Adapter::lagged_short_scope>;
template struct Access<FollowScope, &Adapter::margin_follow_up_scope>;
template struct Access<PolicyBar, &Adapter::policy_script_bar_>;
template struct Access<PolicyValid, &Adapter::policy_script_bar_valid_>;

std::vector<Bar> flat_tape(std::size_t count, std::int64_t step = minute,
                           double volume = 1.0) {
    std::vector<Bar> bars;
    bars.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        bars.push_back({100, 100, 100, 100, volume,
                        start + static_cast<std::int64_t>(i) * step});
    return bars;
}
struct SourceFixture : SourceHost {
    using SourceHost::register_security_eval;
    using SourceHost::register_security_lower_tf_eval;
    using SourceHost::validate_security_timeframes;
    explicit SourceFixture(bool any = true, double margin = 0.0) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 4;
        config.close_entries_rule_any = any;
        config.margin_long = config.margin_short = margin;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.calc_on_order_fills = false;
        config.process_orders_on_close = false;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
        set_syminfo_mintick(0.25);
    }
    void on_source_bar(const Bar&) override { round_trip(static_cast<std::size_t>(pine_bar_index())); }
    TradeLedger& ledger() { return trades_; }
    Adapter& adapter() { return adapter_; }
    void round_trip(std::size_t index) {
        if (index % 3 == 0) strategy_entry("E", true, missing, missing, 1.0);
        if (index % 3 == 1) strategy_close("E");
    }
    auto& pine_state(int id = 0) { return (this->*access(PineStates{})).at(id); }
    auto& state() { return security_eval_states_.at(0); }
    void feed(const Bar& bar) { (this->*access(FeedPine{}))(state(), bar, true); }
};
struct RoundTripHost final : SourceFixture {
    std::size_t calculations = 0;
    void on_source_bar(const Bar&) override { round_trip(calculations++); }
};
void legacy_materialized(const char* row) {
    std::array<std::size_t, 2> remaining{};
    std::size_t trial = 0;
    for (const std::size_t trades : {16U, 64U}) {
        const auto bars = flat_tape(3 * trades);
        RoundTripHost host;
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        if (!ROW(row, host.last_error().empty() && host.ledger().size() == trades,
                 "3T bars submit on 3k, fill on 3k+1 and close on 3k+2, giving T real rows"))
            continue;
        // Warm the ledger's separate decoded cache BEFORE observing legacy
        // owning materializations. No legacy string-pointer call occurs here.
        for (std::size_t i = 0; i < trades; ++i) {
            const Trade value = host.get_trade(static_cast<int>(i));
            ROW(row, value.entry_id == "E" && value.qty == 1.0 && !value.open_at_end,
                "the source submit/fill/close path supplied each materialized input");
        }
        const auto descriptors = host.ledger().size() - host.ledger().resident_base();
        ROW(row, host.ledger().resident_size() == descriptors,
            "the descriptor interval contains no legacy materializations before the C string door");
        const auto before = heap::sample();
        bool ids_match = true;
        for (std::size_t i = 0; i < trades; ++i) {
            const auto* id = strategy_closed_trade_entry_id(
                static_cast<BacktestEngine*>(&host), static_cast<int>(i));
            ids_match = ids_match && id != nullptr && std::strcmp(id, "E") == 0;
        }
        const auto held = heap::sample();
        const auto held_population = host.ledger().resident_size();
        host.ledger().resize(0);
        const auto after = heap::sample();
        ROW(row, ids_match && held_population == descriptors + trades,
            "one C borrowed-string request per distinct closed row inserts exactly T map entries");
        ROW(row, held.bytes > before.bytes && held.blocks > before.blocks,
            "positive population: the owning map was actually allocated");
        ROW(row, heap::same_live(after, before),
            "rewinding the real ledger gives back every legacy map block and byte");
        ROW(row, host.ledger().resident_size() == 0,
            "zero descriptors after rewind leaves resident_size as the actual materialized-map count");
        remaining[trial++] = host.ledger().resident_size();
    }
    ROW(row, trial == 2 && remaining[0] == 0 && remaining[1] == remaining[0],
        "retired legacy occupancy is zero at T and 4T closed trades");
}

// Only fixed scalar/array evidence is changed by evaluator callbacks. There
// is no test-owned vector, string construction, diagnostic, or model work in
// the measured calls. A flat input makes the independent OHLCV model exact.
struct DispatchEvidence {
    std::size_t count = 0;
    double volume = 0.0;
    bool complete = true, flat = true, indices = true, time_order = true;
    std::int64_t first = 0, last = 0;
    void add(const Bar& bar, bool publish, int index) noexcept {
        if (!count) first = bar.timestamp;
        else time_order = time_order && bar.timestamp > last;
        last = bar.timestamp;
        complete = complete && publish;
        flat = flat && bar.open == 100 && bar.high == 100
                    && bar.low == 100 && bar.close == 100;
        indices = indices && index == static_cast<int>(count);
        volume += bar.volume;
        ++count;
    }
};
struct SecurityHost : SourceFixture {
    DispatchEvidence evidence{};
    bool capturing = false;
    void evaluate_security(int id, const Bar& bar, bool complete) override {
        if (capturing && id == 0)
            evidence.add(bar, complete, security_lower_tf_sub_bar_index(0));
    }
    void begin_observation() noexcept { evidence = {}; capturing = true; }
    void end_observation() noexcept { capturing = false; }
    void initialize(const char* input_tf, const char* script_tf) {
        // No evaluator is registered during setup: neither target local can
        // be warmed before its FIRST measured insertion, exposing any retention.
        const bool minute_input = std::strcmp(input_tf, "1") == 0;
        const auto bars = flat_tape(minute_input ? 45 : 3,
                                    minute_input ? minute : 15 * minute);
        run(bars.data(), static_cast<int>(bars.size()), input_tf, script_tf);
    }
};

void direct_security(const char* row, bool synthesize) {
    std::array<std::size_t, 2> unreleased{};
    std::size_t trial = 0;
    for (const std::size_t chunks : {16U, 64U}) {
        SecurityHost host;
        host.initialize(synthesize ? "15" : "1", "15");
        if (!ROW(row, host.last_error().empty() && host.closed_trade_count() == 1, "real setup filled and closed one entry without an evaluator"))
            continue;
        host.register_security_lower_tf_eval(0, "1", synthesize ? "15" : "1");
        host.validate_security_timeframes(synthesize ? "15" : "1");
        ROW(row, host.pine_state().lower_tf_emulation == synthesize
                 && host.pine_state().lower_tf_use_input != synthesize,
            "validation selects the intended synthesis or buffered-input branch");
        const auto first_ms = start + 24 * 60 * minute;
        if (!synthesize) {
            // The first raw bar makes the production reserve, but cannot run
            // dispatch_and_clear: 1 < 15 inputs per script chunk. Each measured
            // cycle then feeds 14 to finish this chunk and 1 to start the next,
            // returning the legitimate persistent buffer to that same point.
            host.feed({100, 100, 100, 100, 1, first_ms});
            ROW(row, host.pine_state().lower_tf_input_buffer.size() == 1,
                "one real input primes the persistent chunk buffer without dispatching the target local");
        }
        std::size_t changed = 0;
        for (std::size_t k = 0; k < chunks; ++k) {
            host.begin_observation();
            const auto before = heap::sample();
            if (synthesize) {
                host.feed({100, 100, 100, 100, 15,
                           first_ms + static_cast<std::int64_t>(k) * 15 * minute});
            } else {
                for (std::size_t j = 1; j <= 15; ++j)
                    host.feed({100, 100, 100, 100, 1,
                               first_ms + static_cast<std::int64_t>(15 * k + j) * minute});
            }
            const auto after = heap::sample();
            host.end_observation();
            const auto& evidence = host.evidence;
            ROW(row, heap::same_live(after, before),
                "the named local's bytes AND blocks are released when the feed call/cycle returns");
            if (!heap::same_live(after, before)) ++changed;
            ROW(row, after.calls > before.calls && evidence.count == 15
                     && evidence.complete && evidence.flat && evidence.indices
                     && evidence.time_order && evidence.volume == 15.0,
                "15 minutes / 1 minute = 15 actual evaluator deliveries and conserved volume");
            ROW(row, evidence.first == first_ms + static_cast<std::int64_t>(15 * k) * minute
                     && evidence.last == evidence.first + 14 * minute,
                "the real dispatch consumed this input interval, rather than a held earlier interval");
            if (!synthesize)
                ROW(row, host.pine_state().lower_tf_input_buffer.size() == 1,
                    "the persistent state returns to one raw input after every full-chunk cycle");
        }
        unreleased[trial++] = changed;
    }
    ROW(row, trial == 2 && unreleased[0] == 0 && unreleased[1] == unreleased[0],
        "zero retained local storage at B=16 and 4B=64 script chunks");
    // Negative control: a same-timeframe scalar site uses neither lower-TF
    // branch. Its one real delivery proves the bypass was driven too.
    SecurityHost control;
    control.initialize("15", "15");
    control.register_security_eval(0, "15", std::string("15"), false, false);
    control.validate_security_timeframes("15");
    control.begin_observation();
    const auto before = heap::sample();
    control.feed({100, 100, 100, 100, 1, start + 48 * 60 * minute});
    const auto after = heap::sample();
    control.end_observation();
    ROW(row, !control.pine_state().lower_tf_use_input && !control.pine_state().lower_tf_emulation
             && control.evidence.count == 1 && heap::same_live(after, before),
        "a scalar same-timeframe input takes the non-lower-TF branch");
}
void buffered_security(const char* row) { direct_security(row, false); }
void synthesized_security(const char* row) { direct_security(row, true); }

struct AuxiliaryHost final : SecurityHost {
    const char* requested;
    bool emulated;
    std::size_t calculations = 0, measured = 0, changed = 0;
    explicit AuxiliaryHost(bool emulate) : requested(emulate ? "15S" : "5"), emulated(emulate) {}
    void on_source_bar(const Bar&) override {
        const auto index = calculations++;
        if (index == 0) {
            // Delay registration until the first actual chart callback. The
            // scheduler's earlier feed had no sites, so no target local was
            // used before the FIRST allocation observation.
            register_security_lower_tf_eval(0, requested, "1");
            validate_security_timeframes("1");
            // This is a DIFFERENT, persistent container (lower_tf_input_buffer), not
            // either target local. Reserve its configured slice population
            // outside the window to isolate return-time release: 15 auxiliary
            // minutes * (60/15 sub-bars if emulated, else one raw bar).
            pine_state().lower_tf_input_buffer.reserve(emulated ? 15 * (60 / 15) : 15);
        }
        begin_observation();
        const auto before = heap::sample();
        (this->*access(FeedAux{}))(static_cast<int>(index));
        const auto after = heap::sample();
        end_observation();
        ++measured;
        if (!heap::same_live(after, before)) ++changed;
        if (emulated) {
            ROW("pinestrategyhost_feed_aux_security_for_chart_bar_synthetic", heap::same_live(after, before),
                "auxiliary synthetic releases bytes AND blocks at this function's return");
            ROW("pinestrategyhost_feed_aux_security_for_chart_bar_requested_bars", heap::same_live(after, before),
                "auxiliary requested_bars releases bytes AND blocks at this function's return");
        } else {
            ROW("pinestrategyhost_feed_aux_security_for_chart_bar_requested_bars", heap::same_live(after, before),
                "the aggregated requested_bars branch releases its allocation on return");
        }
        const auto count = emulated ? 15U * (60U / 15U) : 15U / 5U;
        const bool dispatched = evidence.count == count && evidence.complete && evidence.flat
            && evidence.indices && evidence.time_order && evidence.volume == 15.0
            && evidence.first == start + static_cast<std::int64_t>(index) * 15 * minute
            && evidence.last == evidence.first + (emulated ? 59 * 15000 : 10 * minute);
        ROW("pinestrategyhost_feed_aux_security_for_chart_bar_requested_bars", dispatched && after.calls > before.calls,
            "actual callback rows are the chart slice's 60 synthetic or 3 aggregated bars");
        ROW("pinestrategyhost_feed_aux_security_for_chart_bar_synthetic", pine_state().lower_tf_emulation == emulated
                          && pine_state().lower_tf_use_input != emulated && dispatched,
            "emulation drives synthetic; the 5-minute request is its input-aggregation bypass control");
        // Ordinary real commands accompany the request path. They are outside
        // the allocator window and give one completed trade in both B runs.
        if (index == 0) strategy_entry("E", true, missing, missing, 1.0);
        if (index == 1) strategy_close("E");
    }
};
void auxiliary_security(const char*) {
    std::array<std::size_t, 2> retained{};
    for (const bool emulate : {true, false}) {
        std::size_t trial = 0;
        for (const std::size_t bars : {16U, 64U}) {
            const auto chart = flat_tape(bars, 15 * minute, 15);
            const auto auxiliary = flat_tape(15 * bars);
            AuxiliaryHost host(emulate);
            ROW("pinestrategyhost_feed_aux_security_for_chart_bar_requested_bars", host.set_aux_security_feed(auxiliary.data(),
                    static_cast<int>(auxiliary.size()), "1"), "the public auxiliary feed door accepts the real minute bars");
            host.run(chart.data(), static_cast<int>(chart.size()), "15", "15");
            ROW("pinestrategyhost_feed_aux_security_for_chart_bar_requested_bars", host.last_error().empty() && host.measured == bars
                              && host.calculations == bars && host.closed_trade_count() == 1,
                "B chart callbacks each fed a complete auxiliary slice and an actual trade closed");
            ROW("pinestrategyhost_feed_aux_security_for_chart_bar_synthetic", host.last_error().empty() && host.measured == bars,
                "both the synthetic branch and its bypass were reached on B real chart bars");
            retained[trial++] = host.changed;
        }
        ROW("pinestrategyhost_feed_aux_security_for_chart_bar_requested_bars", retained[0] == 0 && retained[1] == retained[0],
            "requested_bars leaves no retained allocation as chart bars grow from 16 to 64");
        if (emulate)
            ROW("pinestrategyhost_feed_aux_security_for_chart_bar_synthetic", retained[0] == 0 && retained[1] == retained[0],
                "synthetic leaves no retained allocation as chart bars grow from 16 to 64");
    }
}

struct LaggedHost final : SourceFixture {
    std::size_t calculations = 0, partials = 0, changed = 0;
    bool prerequisites = true;
    LaggedHost() : SourceFixture(true, 100.0) { set_syminfo_metadata("qty_step", 0.01); }
    void on_source_bar(const Bar&) override {
        const auto index = calculations++;
        if (index % 4 == 0) strategy_entry("S", false, missing, missing, 10.0);
        if (index % 4 == 1)
            strategy_exit("X", "S", missing, 101.0, missing, missing, missing, 100.0, {}, 1.0);
        if (index % 4 == 2) strategy_close("S", {}, 9.0);
    }
    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext& context) override {
        if (!event.closed_trade_count) return;
        // A real partial stop close supplies the same short-closed-row input
        // the scheduling function reads after a margin close. Ample equity
        // makes its computed additional call zero, so it has no legitimate
        // persistent output to confuse the return-time equality.
        const Trade value = get_trade(static_cast<int>(event.first_trade_index));
        if (value.exit_id != "X") return;
        ++partials;
        const bool ready = event.closed_trade_count == 1 && value.qty == 1.0 && !value.is_long
            && context.coordinate.path_phase == NativePathPhase::High
            && physical_position().signed_units == -9.0
            && (adapter_.*access(CloseScope{}))() && (adapter_.*access(LaggedScope{}))()
            && !(adapter_.*access(FollowScope{}))()
            && adapter_.*access(PolicyValid{})
            && (adapter_.*access(PolicyBar{})).timestamp == context.script_bar_open_ms;
        prerequisites = prerequisites && ready;
        ROW("pineexecutionadapter_schedule_lagged_short_follow_up_taken", ready,
            "real short stop fill, fractional grid, current policy bar and all local-construction guards hold");
        // Warm only the separate row decoder above; do not warm taken itself.
        auto bypass = context;
        bypass.coordinate.path_phase = NativePathPhase::Open;
        const auto bypass_before = heap::sample();
        (adapter_.*access(ScheduleLagged{}))(event, bypass);
        const auto bypass_after = heap::sample();
        ROW("pineexecutionadapter_schedule_lagged_short_follow_up_taken", heap::same_live(bypass_after, bypass_before)
                          && bypass_after.calls == bypass_before.calls,
            "the Open-phase control returns before taken is built");
        const auto before = heap::sample();
        (adapter_.*access(ScheduleLagged{}))(event, context);
        const auto after = heap::sample();
        ROW("pineexecutionadapter_schedule_lagged_short_follow_up_taken", heap::same_live(after, before),
            "taken is released in bytes AND blocks when the actual scheduling function returns");
        ROW("pineexecutionadapter_schedule_lagged_short_follow_up_taken", ready && after.calls > before.calls,
            "the closed short row really enters the allocating taken branch");
        if (!heap::same_live(after, before)) ++changed;
    }
};
void lagged_short(const char* row) {
    std::array<std::size_t, 2> retained{};
    std::size_t trial = 0;
    for (const std::size_t cycles : {16U, 64U}) {
        auto bars = flat_tape(4 * cycles, 15 * minute);
        for (std::size_t i = 2; i < bars.size(); i += 4) {
            bars[i].high = 102;
            bars[i].low = 99;
        }
        LaggedHost host;
        host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
        ROW(row, host.last_error().empty() && host.partials == cycles && host.prerequisites
                 && host.closed_trade_count() == 2 * cycles,
            "each four-bar cycle really closes 1 unit by stop and 9 by market: T=32 or 128 closed trades");
        retained[trial++] = host.changed;
    }
    ROW(row, retained[0] == 0 && retained[1] == retained[0],
        "zero retained taken storage at T and 4T actual closed trades");
}

struct GlobalExitHost final : SourceFixture {
    bool two_lots;
    std::size_t calculations = 0, closed_events = 0, annotations = 0;
    bool recut = true;
    explicit GlobalExitHost(bool two) : SourceFixture(true), two_lots(two) {}
    void on_source_bar(const Bar&) override {
        const auto index = calculations++;
        // Separate entry bars force lot order Z,A, independently of the
        // adapter's same-point pending-entry ordering rule.
        if (index == 0) strategy_entry("Z", true, missing, missing, 1.0);
        if (index == 1 && two_lots) strategy_entry("A", true, missing, missing, 1.0);
        if (index == 2) strategy_exit("G", "", 101.0, missing);
    }
    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        if (!event.closed_trade_count) return;
        ++closed_events;
        auto* replay = report_replay_state();
        if (!replay || !replay->begun()) { recut = false; return; }
        // Z: Java hash 90 -> bucket 10 of 16. A: 65 -> bucket 1.
        // Entry order Z,A is therefore replaced by A,Z, without asking the
        // product rank helper to compute our expectation. ANY keeps one row
        // per lot: two children, slices 0 and 1, child ordinals 1 and 0.
        for (std::size_t i = 0; i < event.closed_trade_count; ++i) {
            const auto annotation = replay->trade(event.first_trade_index + i);
            const Trade trade = get_trade(static_cast<int>(event.first_trade_index + i));
            if (two_lots) {
                recut = recut && event.closed_trade_count == 2 && annotation.marked
                    && annotation.parent == event.ordinal && annotation.slice == i
                    && annotation.slices == 2 && annotation.child == 1 - i
                    && annotation.transaction_qty == 1.0 && !annotation.fifo
                    && trade.entry_id == (i == 0 ? "A" : "Z") && trade.qty == 1.0;
                if (annotation.marked) ++annotations;
            } else {
                recut = recut && !annotation.marked && trade.entry_id == "Z";
            }
        }
    }
};
struct GlobalResult {
    bool ok = false, recut = false;
    std::size_t trades = 0, annotations = 0, closed_events = 0;
};
GlobalResult global_cycle(const std::array<Bar, 4>& bars, const std::string& path,
                          bool two_lots) {
    // This path creates persistent trades and replay rows. The repeatable
    // cycle ends by releasing their run owner, returning persistent state to
    // its original empty point. Input/path storage predates the measurement.
    GlobalExitHost host(two_lots);
    const auto installed = host.set_report_replay_v1(path.c_str(), "feed-replay");
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    return {installed == 0 && host.last_error().empty(), host.recut,
            host.closed_trade_count(), host.annotations, host.closed_events};
}
void global_replay(const char* row) {
    std::array<Bar, 4> bars{{{100, 100, 100, 100, 1, start},
                             {100, 100, 100, 100, 1, start + minute},
                             {100, 100, 100, 100, 1, start + 2 * minute},
                             {100, 102, 99, 100, 1, start + 3 * minute}}};
    std::array<char, 64> directory{};
    const char pattern[] = "/tmp/pineforge-feed-replay-XXXXXX";
    std::copy(std::begin(pattern), std::end(pattern), directory.begin());
    if (!ROW(row, ::mkdtemp(directory.data()) != nullptr,
             "a unique companion directory is prepared outside every allocation window")) return;
    struct Cleanup {
        const char* path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory.data()};
    std::array<std::string, 81> paths;
    for (std::size_t i = 0; i < paths.size(); ++i)
        paths[i] = std::string(directory.data()) + "/recut-" + std::to_string(i) + ".ndjson";
    // Prime only the registry/writer/common run setup. count=1 returns BEFORE
    // replay_rows is declared, so static retention cannot be hidden in
    // a warm-up. This also drives the required non-recut control branch.
    const auto control = global_cycle(bars, paths[0], false);
    ROW(row, control.ok && control.recut && control.trades == 1
             && control.annotations == 0 && control.closed_events == 1,
        "the single-lot global exit bypasses the recut local and closes one real lot");
    std::size_t path = 1;
    std::array<std::size_t, 2> changed{};
    std::size_t trial = 0;
    for (const std::size_t cycles : {16U, 64U}) {
        const auto baseline = heap::sample();
        heap::Sample first_after{};
        std::size_t retained = 0, annotations = 0, closed = 0;
        for (std::size_t i = 0; i < cycles; ++i) {
            const auto result = global_cycle(bars, paths[path++], true);
            const auto after = heap::sample();
            if (i == 0) first_after = after;
            ROW(row, heap::same_live(after, baseline),
                "the completed submit/fill/recut/owner-release cycle returns all bytes AND blocks");
            ROW(row, heap::same_live(after, first_after),
                "cycles k=1..N keep the same persistent end state as cycle 1");
            ROW(row, result.ok && result.recut && result.trades == 2
                     && result.annotations == 2 && result.closed_events == 1,
                "marked replay annotations and A,Z rows prove order_global_exit_children reached its local append");
            if (!heap::same_live(after, baseline)) ++retained;
            annotations += result.annotations;
            closed += result.trades;
        }
        ROW(row, annotations == 2 * cycles && closed == 2 * cycles,
            "two real closed lots and two local-backed annotations per cycle give T=32 or 128");
        changed[trial++] = retained;
    }
    ROW(row, changed[0] == 0 && changed[1] == changed[0],
        "nothing the recut path allocated outlives its host at T and 4T actual closed trades");
}

NativeRunSpec native_spec(std::uint64_t run) {
    NativeRunSpec spec;
    spec.identity = {"feed-report-cycle", run};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:FEED";
    spec.type = "crypto";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 1000000.0;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.25;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    return spec;
}
struct NativeRoundTrip final : NativeStrategyHost {
    bool trading = true, accepted = true;
    std::size_t calculations = 0;
    explicit NativeRoundTrip(bool trade = true) : trading(trade) {}
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const auto index = calculations++;
        if (!trading || index % 3 == 2) return;
        no::Request request;
        if (index % 3 == 0) { request.intent = no::Transact{1.0}; request.label = "E"; }
        else { request.intent = no::Flatten{}; request.label = "C"; }
        const auto result = submit(request);
        accepted = accepted && result.status == no::SubmitStatus::Accepted && result.handle.has_value();
    }
    std::size_t closed() const { return trades_.size(); }
};
struct PatchEvidence {
    std::size_t rows = 0, offset = 0, logical = 0, closed = 0;
};
struct AccumulatorEvidence {
    std::size_t trades = 0, points = 0, point_rows = 0;
    std::uint64_t fallbacks = 0;
};
AccumulatorEvidence accumulator_cycle(const BacktestEngine& host) {
    // A fresh accumulator consumes the real prefix, then releases its State.
    // The engine prefix is already present and unchanged between cycles. Both
    // the public patch and the accumulator are destroyed before return.
    LiveReportAccumulator accumulator;
    const auto patch = accumulator.update(host, 0);
    return {patch.closed_trades, patch.equity.logical_length,
            patch.equity.rows.size(), accumulator.work().fallback_report_reads};
}
void accumulator_curves(const char*) {
    std::array<std::size_t, 2> retained{};
    std::size_t trial = 0;
    for (const std::size_t trades : {16U, 64U}) {
        const auto bars = flat_tape(3 * trades);
        NativeRoundTrip host;
        const auto configured = host.configure_native(native_spec(3000 + trades));
        const bool began = host.stream_begin(bars.data(), static_cast<int>(bars.size()), "1", "1");
        const bool real_prefix = configured.status == NativeSetupStatus::Applied
            && began && host.accepted && host.closed() == trades;
        ROW("livereportaccumulator_state_curve", real_prefix, "a real native realtime prefix supplies T trades and B=3T bars");
        ROW("livereportaccumulator_state_ratio_curve", real_prefix, "the same actual realtime prefix supplies the ratio workspace");
        if (!real_prefix) continue;
        // Prime separate engine-owned decoded/report state without ever
        // constructing or updating a LiveReportAccumulator. In particular a
        // retained static ratio-curve storage cannot be primed before its first check.
        for (std::size_t i = 0; i < trades; ++i) {
            const Trade value = host.get_trade(static_cast<int>(i));
            ROW("livereportaccumulator_state_curve", value.qty == 1.0 && !value.open_at_end,
                "every decoded input row came from a real completed fill/close");
        }
        {
            struct ReportGuard {
                ReportC value{};
                ~ReportGuard() { BacktestEngine::free_report(&value); }
            } warm;
            host.fill_report(&warm.value);
            const bool populated = warm.value.equity_curve_len == static_cast<std::int64_t>(bars.size());
            ROW("livereportaccumulator_state_curve", populated, "the independent engine report contains one point per actual script bar");
            ROW("livereportaccumulator_state_ratio_curve", populated, "the ratio input population is B=48 or 192 actual equity points");
        }
        const auto hash_before = host.stream_state_hash();
        const auto baseline = heap::sample();
        heap::Sample first_after{};
        std::size_t changed = 0;
        for (std::size_t k = 0; k < trades; ++k) {
            const auto result = accumulator_cycle(host);
            const auto after = heap::sample();
            if (!k) first_after = after;
            ROW("livereportaccumulator_state_curve", heap::same_live(after, baseline),
                "the curve's complete accumulator-owner cycle returns its bytes AND blocks");
            ROW("livereportaccumulator_state_ratio_curve", heap::same_live(after, baseline),
                "the ratio curve's complete accumulator-owner cycle returns its bytes AND blocks");
            ROW("livereportaccumulator_state_curve", heap::same_live(after, first_after),
                "curve owner cycles k=1..N have the same persistent end state as cycle 1");
            ROW("livereportaccumulator_state_ratio_curve", heap::same_live(after, first_after),
                "ratio workspace owner cycles k=1..N have the same persistent end state as cycle 1");
            const bool reached = result.fallbacks == 0 && result.trades == trades
                && result.points == bars.size() && result.point_rows == bars.size();
            ROW("livereportaccumulator_state_curve", reached,
                "ordinary-live dispatch visits all B points, executing the curve append for each one");
            ROW("livereportaccumulator_state_ratio_curve", reached,
                "that same ordinary-live loop appends each of the B points to the ratio workspace");
            const auto hash_after = host.stream_state_hash();
            ROW("livereportaccumulator_state_curve", hash_after == hash_before, "the engine's persistent continuation is unchanged by the owner cycle");
            ROW("livereportaccumulator_state_ratio_curve", hash_after == hash_before, "ratio workspace release also leaves the engine continuation unchanged");
            if (!heap::same_live(after, baseline)) ++changed;
        }
        retained[trial++] = changed;
    }
    ROW("livereportaccumulator_state_curve", trial == 2 && retained[0] == 0 && retained[1] == retained[0],
        "zero retained owner storage after B=48 and 4B=192 points and N=16 or 4N=64 owner cycles");
    ROW("livereportaccumulator_state_ratio_curve", trial == 2 && retained[0] == 0 && retained[1] == retained[0],
        "ratio workspace storage is released at the same two bar populations and owner boundaries");
    // The completed-batch fallback supplies a full public patch but bypasses
    // both ordinary-live append sites. Keep it AFTER the positive lifetime
    // checks so constructing its State cannot prime retained static storage.
    const auto control_bars = flat_tape(3);
    NativeRoundTrip control(false);
    const auto configured = control.configure_native(native_spec(3999));
    control.run(control_bars.data(), static_cast<int>(control_bars.size()));
    const auto bypass = accumulator_cycle(control);
    const bool fallback = configured.status == NativeSetupStatus::Applied
        && control.last_error().empty() && bypass.fallbacks == 1
        && bypass.trades == 0 && bypass.points == control_bars.size();
    ROW("livereportaccumulator_state_curve", fallback, "a completed-batch read takes the fallback before the curve append loop");
    ROW("livereportaccumulator_state_ratio_curve", fallback, "the same completed-batch fallback bypasses the ratio workspace append loop");
}
PatchEvidence read_live_patch(LiveReportAccumulator& accumulator, const BacktestEngine& host) {
    // The patch is legitimate returned output; destroy it before sampling
    // the end of the call/caller cycle. The evidence itself owns no storage.
    const auto patch = accumulator.update(host, 0);
    return {patch.trades.rows.size(), patch.trades.offset,
            patch.trades.logical_length, patch.closed_trades};
}
void live_trade_scratch(const char* row) {
    std::array<std::size_t, 2> suffix_rows{};
    std::size_t trial = 0;
    for (const std::size_t trades : {16U, 64U}) {
        const auto bars = flat_tape(3 * trades);
        NativeRoundTrip host;
        if (!ROW(row, host.configure_native(native_spec(trades)).status == NativeSetupStatus::Applied,
                 "the native FULL report configuration is admitted")) continue;
        if (!ROW(row, host.stream_begin(bars.data(), static_cast<int>(bars.size()), "1", "1")
                     && host.accepted && host.closed() == trades,
                 "a real realtime stream owns T completed native submit/fill/close rows")) continue;
        LiveReportAccumulator accumulator;
        const auto warm = read_live_patch(accumulator, host);
        ROW(row, warm.closed == trades && warm.rows == trades
                 && accumulator.work().fallback_report_reads == 0,
            "the first ordinary-live update fills trade_rows with the complete real closed prefix");
        const auto baseline = heap::sample();
        heap::Sample first_after{};
        PatchEvidence last{};
        for (std::size_t k = 0; k < trades; ++k) {
            last = read_live_patch(accumulator, host);
            const auto after = heap::sample();
            if (k == 0) first_after = after;
            ROW(row, heap::same_live(after, baseline) && heap::same_live(after, first_after),
                "steady ordinary-live updates retain the warm-up bytes AND blocks, never append prior scratch rows");
            ROW(row, last.rows == 1 && last.offset == trades - 1 && last.logical == trades
                     && last.closed == trades && accumulator.work().fallback_report_reads == 0,
                "exactly one unsealed last row is rebuilt for a stable T-row prefix");
        }
        suffix_rows[trial++] = last.rows;
    }
    ROW(row, trial == 2 && suffix_rows[0] == 1 && suffix_rows[1] == suffix_rows[0],
        "the rebuilt logical scratch suffix stays one row at T=16 and 4T=64 closed trades");
    // A no-trade stream is a public-call control for the empty scratch path.
    const auto bars = flat_tape(3);
    NativeRoundTrip control(false);
    const auto configured = control.configure_native(native_spec(1000));
    const bool began = control.stream_begin(bars.data(), static_cast<int>(bars.size()), "1", "1");
    LiveReportAccumulator empty;
    const auto zero = read_live_patch(empty, control);
    ROW(row, configured.status == NativeSetupStatus::Applied && began
             && zero.rows == 0 && zero.closed == 0 && empty.work().fallback_report_reads == 0,
        "the empty prefix produces no trade scratch entries through the same ordinary-live function");
}

struct HashHost final : SourceFixture {
    std::size_t calculations = 0, positive = 0, changed = 0;
    HashHost() {
        source::StrategyOverrides overrides;
        overrides.process_orders_on_close = 1;
        set_strategy_override(overrides);
    }
    void on_source_bar(const Bar&) override {
        const auto index = calculations++;
        // The empty-id close takes the adapter's ordinary POOC CloseAll
        // freeze path before flattening. Refill the same physical lots so
        // later callbacks observe both the retained snapshot and the book.
        if (index == 1) strategy_close("");
        if (index < 2) {
            strategy_entry("B", true, missing, missing, 2.0);
            strategy_entry("A", true, missing, missing, 1.0);
            return;
        }
        auto& scheduler = this->*access(SchedulerMember{});
        const auto& map = scheduler.language().*access(FrozenEntries{});
        const auto a = map.find("A"), b = map.find("B");
        const bool real_lots = map.size() == 2 && a != map.end() && b != map.end()
            && a->second == 1.0 && b->second == 2.0 && physical_position().signed_units == 3.0;
        BrokerStateHashSink expected;
        // Independent lexical model, deliberately opposite the submitted
        // order B,A; only the generic sink primitive is shared with product.
        expected.u(2); expected.s("A"); expected.d(1); expected.s("B"); expected.d(2);
        BrokerStateHashSink actual;
        const auto before = heap::sample();
        hash_str_double_map(actual, map);
        const auto after = heap::sample();
        ROW("broker_state_hash_copies", heap::same_live(after, before),
            "the reachable aggregate component hash_str_double_map::v releases its bytes AND blocks");
        ROW("broker_state_hash_copies", real_lots && actual.h == expected.h && after.calls > before.calls,
            "the real scheduler's frozen two-entry lot map is copied and folded in lexical order");
        if (!heap::same_live(after, before)) ++changed;
        ++positive;
    }
};
void reachable_hash_component(const char* row) {
    // This checks one reachable component, not the complete hash
    // aggregate: its other two helpers have no production caller.
    std::array<std::size_t, 2> retained{};
    std::size_t trial = 0;
    for (const std::size_t calls : {16U, 64U}) {
        const auto bars = flat_tape(calls + 2);
        HashHost host;
        host.set_broker_state_hash_recording(true);
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        ROW(row, host.last_error().empty() && host.positive == calls,
            "B and 4B real script bars each carry the frozen open-lot map to the production hash helper");
        retained[trial++] = host.changed;
    }
    ROW(row, retained[0] == 0 && retained[1] == retained[0],
        "the production-reachable copy retains zero storage as hashed bars grow 16 to 64");
}

struct BatchReport {
    ReportC value{};
    ~BatchReport() { BacktestEngine::free_report(&value); }
};
void refreshed_ratios(const char* row) {
    using live::Json;
    const std::string zone = "UTC";
    const std::array<std::string, 4> keys{{"sharpe_monthly", "sortino_monthly", "sharpe_bar", "sortino_bar"}};
    std::array<std::size_t, 2> changed{};
    std::size_t trial = 0;
    for (const std::size_t trades : {16U, 64U}) {
        const auto bars = flat_tape(3 * trades);
        NativeRoundTrip host;
        if (!ROW(row, host.configure_native(native_spec(2000 + trades)).status == NativeSetupStatus::Applied,
                 "the real report producer is configured")) continue;
        host.run(bars.data(), static_cast<int>(bars.size()));
        BatchReport report;
        host.fill_report(&report.value);
        if (!ROW(row, host.last_error().empty() && host.accepted && host.closed() == trades
                     && report.value.equity_curve_len == static_cast<std::int64_t>(bars.size()),
                 "3T flat-price script bars produce T real closed trades and B=3T equity points")) continue;
        Json document = Json::object({
            {"report", Json::object({
                {"equity_curve", Json{}},
                {"metrics", Json::object({{"equity", Json::object({})}})}
            })}
        });
        auto& points = document.members.at("report").members.at("equity_curve");
        points.kind = Json::Kind::Array;
        for (std::int64_t i = 0; i < report.value.equity_curve_len; ++i) {
            const auto& point = report.value.equity_curve[i];
            points.items.push_back(Json::object({{"time_ms", live::report_number(point.time_ms)},
                {"equity", live::report_number(point.equity)},
                {"open_profit", live::report_number(point.open_profit)}}));
        }
        auto& fields = document.members.at("report").members.at("metrics").members.at("equity");
        // Preparing the legitimate persistent output is separate from the
        // local's release proof. Call the metrics primitive directly (NOT
        // refresh_report_ratios) and format its four fields outside the
        // window. Thus retained static target-curve storage cannot be primed,
        // and changing a legitimate output string's length cannot imitate a
        // leak. No numeric result here is an expected allocation budget.
        // metrics_stream.hpp declares the generic compute_equity_stats
        // template; engine_metrics.cpp instantiates ArrayCurve, not a mutable
        // pointer specialization. Match refresh_report_ratios' const pointer
        // to select metrics.hpp's ordinary compute_equity_stats overload.
        const pf_equity_point_t* curve_points = report.value.equity_curve;
        const auto prepared = metrics::compute_equity_stats(curve_points,
            report.value.equity_curve_len, 1000000.0, zone, missing, missing, 0, 0.0);
        fields.members[keys[0]] = live::report_number(prepared.sharpe_monthly);
        fields.members[keys[1]] = live::report_number(prepared.sortino_monthly);
        fields.members[keys[2]] = live::report_number(prepared.sharpe_bar);
        fields.members[keys[3]] = live::report_number(prepared.sortino_bar);
        const Json expected_fields = fields;
        const auto point_count = points.items.size();
        std::size_t retained = 0;
        for (std::size_t k = 0; k < trades; ++k) {
            // assign preserves the pre-existing string capacity. Restoring
            // all four fields is an independent observable that the function
            // actually parsed and consumed its equity array on this call.
            for (const auto& key : keys) {
                auto& field = fields.members.at(key);
                field.kind = Json::Kind::String;
                field.value.assign("pending");
            }
            const auto before = heap::sample();
            live::refresh_report_ratios(document, zone);
            const auto after = heap::sample();
            ROW(row, heap::same_live(after, before),
                "refresh_report_ratios::curve gives back its bytes AND blocks when the real function returns");
            bool restored = true;
            for (const auto& key : keys) {
                const auto& value = fields.members.at(key);
                const auto& expected = expected_fields.members.at(key);
                restored = restored && value.kind == expected.kind && value.value == expected.value;
            }
            ROW(row, restored && after.calls > before.calls && point_count == 3 * trades
                     && points.items.size() == point_count,
                "all four overwritten ratio outputs are restored after parsing B real report equity points");
            if (!heap::same_live(after, before)) ++retained;
        }
        changed[trial++] = retained;
    }
    ROW(row, trial == 2 && changed[0] == 0 && changed[1] == changed[0],
        "the transient curve leaves zero storage at B=48 and 4B=192 reported bars");
}

void allocation_forms(const char* row) {
    // An allocator self-check, not a container proof. Explicit calls avoid permitted
    // new-expression elision. Sixteen bytes is a fixture payload, not a
    // container allowance. Every C++17 replaceable form uses the same prefix.
    const auto before = heap::sample();
    void* a = ::operator new(16);
    void* b = ::operator new[](16);
    void* c = ::operator new(16, std::align_val_t{64});
    void* d = ::operator new[](16, std::align_val_t{64});
    void* e = ::operator new(16, std::nothrow);
    void* f = ::operator new[](16, std::nothrow);
    void* g = ::operator new(16, std::align_val_t{64}, std::nothrow);
    void* h = ::operator new[](16, std::align_val_t{64}, std::nothrow);
    const auto held = heap::sample();
    ::operator delete(a);
    ::operator delete[](b, std::size_t{16});
    ::operator delete(c, std::align_val_t{64});
    ::operator delete[](d, std::size_t{16}, std::align_val_t{64});
    ::operator delete(e, std::nothrow);
    ::operator delete[](f, std::nothrow);
    ::operator delete(g, std::align_val_t{64}, std::nothrow);
    ::operator delete[](h, std::align_val_t{64}, std::nothrow);
    const auto after = heap::sample();
    ROW(row, held.bytes == before.bytes + 8 * 16 && held.blocks == before.blocks + 8
             && held.calls == before.calls + 8 && heap::same_live(after, before),
        "eight allocating forms are charged once each and all matching release forms return to baseline");
}

struct RowCase {
    const char* first;
    const char* second;
    void (*run)(const char*);
};
} // namespace

int main() {
    feed_report_heap::test_thread = true;
    const RowCase cases[]{
        {"allocator-release", nullptr, allocation_forms},
        {"tradeledger_state_materialized", nullptr, legacy_materialized},
        {"pineexecutionadapter_schedule_lagged_short_follow_up_taken", nullptr, lagged_short},
        {"pinestrategyhost_feed_aux_security_for_chart_bar_synthetic", "pinestrategyhost_feed_aux_security_for_chart_bar_requested_bars", auxiliary_security},
        {"pinestrategyhost_pine_feed_security_eval_state_ltf_bars", nullptr, buffered_security},
        {"pinestrategyhost_pine_feed_security_eval_state_synthetic_bars", nullptr, synthesized_security},
        {"pinestrategyhost_order_global_exit_children_replay_rows", nullptr, global_replay},
        {"livereportaccumulator_state_curve", "livereportaccumulator_state_ratio_curve", accumulator_curves},
        {"livereportaccumulator_state_trade_rows", nullptr, live_trade_scratch},
        {"broker_state_hash_copies", nullptr, reachable_hash_component},
        {"refresh_report_ratios_curve", nullptr, refreshed_ratios},
    };
    for (const auto& item : cases) {
        try {
            item.run(item.first);
        } catch (const std::exception& error) {
            ROW(item.first, false, error.what());
            if (item.second) ROW(item.second, false, error.what());
        } catch (...) {
            ROW(item.first, false, "unexpected exception; row dispatch continues");
            if (item.second) ROW(item.second, false, "unexpected exception; row dispatch continues");
        }
    }
    std::printf("memory proof release_feeds_reports: %zu checks, %zu failures\n", checks, failures);
    return failures ? 1 : 0;
}
