// Views and scratch buffers release storage at their ownership boundaries.
// The expectations are ownership equalities, not measured byte constants.
// Keep this in its own executable: it replaces every replaceable C++17
// global allocation/deallocation form, following global_allocation_replacement.hpp.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/exit_scratch.hpp>
#include <pineforge/pineforge.h>

#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace view_allocation {
// Attribution follows the allocating thread even if another thread releases
// the block. A foreign-thread allocation OR release during a measurement is
// an explicit failed prerequisite, rather than silently excluded work.
thread_local bool test_thread = false;
std::atomic<bool> window_open{false};
std::atomic<std::size_t> live_bytes{0}, live_blocks{0}, allocations{0};
std::atomic<std::size_t> foreign_events{0};

struct Header {
    std::size_t size;
    std::size_t offset;
    bool counted;
};
constexpr std::size_t kFundamental = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
constexpr std::size_t kPrefix =
    (sizeof(Header) + kFundamental - 1) / kFundamental * kFundamental;

void foreign_event() noexcept {
    if (!test_thread && window_open.load(std::memory_order_relaxed))
        foreign_events.fetch_add(1, std::memory_order_relaxed);
}

void* allocate(std::size_t size, std::size_t alignment) noexcept {
    foreign_event();
    if (size == 0) size = 1;
    const std::size_t prefix = std::max(kPrefix, alignment);
    if (size > std::numeric_limits<std::size_t>::max() - prefix) return nullptr;
    void* base = nullptr;
    if (alignment <= kFundamental) base = std::malloc(prefix + size);
    else if (::posix_memalign(&base, alignment, prefix + size) != 0) return nullptr;
    if (!base) return nullptr;
    auto* block = static_cast<unsigned char*>(base) + prefix;
    const Header header{size, prefix, test_thread};
    std::memcpy(block - sizeof(Header), &header, sizeof(header));
    if (header.counted) {
        live_bytes.fetch_add(size, std::memory_order_relaxed);
        live_blocks.fetch_add(1, std::memory_order_relaxed);
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
    return block;
}

void release(void* block) noexcept {
    if (!block) return;
    foreign_event();
    auto* bytes = static_cast<unsigned char*>(block);
    Header header{};
    std::memcpy(&header, bytes - sizeof(Header), sizeof(header));
    if (header.counted) {
        // Counters are never reset while attributed blocks are alive.
        live_bytes.fetch_sub(header.size, std::memory_order_relaxed);
        live_blocks.fetch_sub(1, std::memory_order_relaxed);
    }
    std::free(bytes - header.offset);
}

void* throwing(std::size_t size, std::size_t alignment = kFundamental) {
    for (;;) {
        if (void* block = allocate(size, alignment)) return block;
        const auto handler = std::get_new_handler();
        if (!handler) throw std::bad_alloc();
        handler();
    }
}
void* nonthrowing(std::size_t size, std::size_t alignment = kFundamental) noexcept {
    try { return throwing(size, alignment); }
    catch (...) { return nullptr; }
}
} // namespace view_allocation

void* operator new(std::size_t n) { return view_allocation::throwing(n); }
void* operator new[](std::size_t n) { return view_allocation::throwing(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return view_allocation::nonthrowing(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return view_allocation::nonthrowing(n);
}
void* operator new(std::size_t n, std::align_val_t a) {
    return view_allocation::throwing(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
    return view_allocation::throwing(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return view_allocation::nonthrowing(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return view_allocation::nonthrowing(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { view_allocation::release(p); }
void operator delete[](void* p) noexcept { view_allocation::release(p); }
void operator delete(void* p, std::size_t) noexcept { view_allocation::release(p); }
void operator delete[](void* p, std::size_t) noexcept { view_allocation::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { view_allocation::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { view_allocation::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { view_allocation::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { view_allocation::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    view_allocation::release(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    view_allocation::release(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    view_allocation::release(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    view_allocation::release(p);
}

namespace {
using namespace pineforge;
namespace ps = pineforge::source;
namespace sd = pineforge::source::detail;
namespace pd = pineforge::detail;
using Host = ps::PineStrategyHost;
using Consumer = NativeExecutionConsumer;
using Status = pd::ScratchStatus;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct StageViews { friend auto access(StageViews); };
struct PendingViews { friend auto access(PendingViews); };
struct StagedFx { friend auto access(StagedFx); };
struct FailureRecord { friend auto access(FailureRecord); };
template struct Access<StageViews, &Host::stage_symbol_feed_views>;
template struct Access<PendingViews, &Consumer::pending_bar_views_>;
template struct Access<StagedFx, &Consumer::staged_fx_curve_>;
template struct Access<FailureRecord, &sd::ExitScratchTeardownFailure::failure_>;

// A static-duration control changes the member spelling to an inline static
// field. Accept either pointer kind so that control still compiles and is
// judged by the same runtime release assertion.
template<class Owner, class Pointer>
decltype(auto) failure_record(Owner& owner, Pointer pointer) {
    if constexpr (std::is_member_object_pointer_v<Pointer>) return owner.*pointer;
    else return *pointer;
}

std::size_t checks = 0, failures = 0;
void check(const char* row, bool value, const char* expression, int line) {
    ++checks;
    if (value) return;
    ++failures;
    std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", row, __FILE__, line, expression);
}
#define ROW_CHECK(row, expression) check(row, (expression), #expression, __LINE__)

constexpr std::size_t kN = 16;
constexpr std::size_t kLong = 4 * kN;
constexpr std::size_t kFeeds = 3;
constexpr std::size_t kFeedBars = 8;
constexpr std::size_t kFxPoints = 16;
constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kStart = 1704067200000LL; // 2024-01-01 UTC

struct Counts {
    std::size_t bytes = 0, blocks = 0, allocations = 0, foreign = 0;
};
Counts counts() noexcept {
    return {view_allocation::live_bytes.load(std::memory_order_relaxed),
            view_allocation::live_blocks.load(std::memory_order_relaxed),
            view_allocation::allocations.load(std::memory_order_relaxed),
            view_allocation::foreign_events.load(std::memory_order_relaxed)};
}
bool same_live(Counts a, Counts b) noexcept {
    return a.bytes == b.bytes && a.blocks == b.blocks;
}
struct Observation {
    Counts before, after;
    bool returned = false;
};
template<class Call> Observation observe(Call call) {
    Observation result;
    result.before = counts();
    view_allocation::window_open.store(true, std::memory_order_relaxed);
    try { call(); result.returned = true; }
    catch (...) { /* Dispatch records this after the allocation window. */ }
    view_allocation::window_open.store(false, std::memory_order_relaxed);
    result.after = counts();
    return result;
}
void check_release(const char* row, const char* phase, std::size_t index,
                   const Observation& sample) {
    ROW_CHECK(row, sample.returned);
    ROW_CHECK(row, sample.after.foreign == sample.before.foreign);
    ROW_CHECK(row, sample.after.bytes == sample.before.bytes);
    ROW_CHECK(row, sample.after.blocks == sample.before.blocks);
    if (!same_live(sample.before, sample.after) || !sample.returned)
        std::fprintf(stderr,
            "DETAIL [%s] %s[%zu]: live bytes %zu -> %zu, blocks %zu -> %zu\n",
            row, phase, index, sample.before.bytes, sample.after.bytes,
            sample.before.blocks, sample.after.blocks);
}

template<std::size_t N> std::array<Bar, N> tape() {
    std::array<Bar, N> result{};
    for (std::size_t i = 0; i < N; ++i)
        result[i] = {100.0, 101.0, 99.0, 100.0, 1.0,
                     kStart + static_cast<std::int64_t>(i) * kMinute};
    return result;
}

class TradingHost final : public Host {
public:
    TradingHost() {
        ps::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        config.src_series_active = false;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
        set_syminfo_mintick(0.01);
    }
    void on_source_bar(const Bar&) override {
        ++seen;
        const int index = pine_bar_index();
        if (index % 8 == 0) strategy_entry("view-position", true);
        if (index % 8 == 3) strategy_close("view-position");
    }
    int seen = 0;
};
struct RunEvidence {
    bool returned = false, clean = false;
    std::size_t bars = 0, trades = 0;
};
RunEvidence run_source(TradingHost& host, const Bar* bars, std::size_t n) {
    RunEvidence result;
    host.seen = 0;
    try {
        host.run(bars, static_cast<int>(n), "1", "1", false);
        result.returned = true;
    } catch (...) {}
    result.clean = host.last_error().empty();
    result.bars = static_cast<std::size_t>(host.seen);
    result.trades = static_cast<std::size_t>(host.trade_count());
    return result;
}

struct FeedInput {
    std::array<Bar, kFeedBars> bars = tape<kFeedBars>();
    std::array<std::int64_t, kFeedBars> closes{};
    std::array<NativeInstrumentFeedView, kFeeds> descriptors{};
    std::array<const NativeInstrumentFeedView*, kFeeds> slots{};
    FeedInput() {
        for (std::size_t i = 0; i < bars.size(); ++i)
            closes[i] = bars[i].timestamp + kMinute;
        for (std::size_t i = 0; i < kFeeds; ++i) {
            // A valid one-row receiving view has the same allocation shape
            // as the eight-row source view, but observably different content.
            descriptors[i] = {{bars.data(), 1}, closes.data()};
            slots[i] = &descriptors[i];
        }
    }
};
bool pending_matches(const TradingHost& host, const FeedInput& input) {
    const auto& pending = Consumer::bound(host).*access(PendingViews{});
    if (pending.descriptors.size() != kFeeds || pending.slots.size() != kFeeds)
        return false;
    for (std::size_t i = 0; i < kFeeds; ++i) {
        const auto& descriptor = pending.descriptors[i];
        if (!descriptor || pending.slots[i] != &*descriptor
            || descriptor->bars.data != input.bars.data()
            || descriptor->bars.size != input.bars.size()
            || descriptor->close_ms != input.closes.data()) return false;
    }
    return true;
}
template<std::size_t Calls> struct FeedSeries {
    std::array<Observation, Calls> samples{};
    std::size_t correct_transports = 0;
};
template<std::size_t Calls>
FeedSeries<Calls> stage_series(TradingHost& host, const FeedInput& input) {
    FeedSeries<Calls> result;
    for (std::size_t i = 0; i < Calls; ++i) {
        // Prime only the receiver, before EVERY measured call. In particular,
        // this does not warm either source-local vector on its first use.
        Consumer::bound(host).stage_instrument_feed_views(input.slots.data(), kFeeds);
        const bool different_before = !pending_matches(host, input);
        result.samples[i] = observe([&] { (host.*access(StageViews{}))(); });
        if (different_before && pending_matches(host, input)) ++result.correct_transports;
    }
    return result;
}
struct FeedResults {
    Observation empty;
    bool empty_transport = false, installed = true;
    FeedSeries<kN> first, short_run;
    FeedSeries<kLong> long_run;
    RunEvidence short_bars, long_bars;
};
FeedResults exercise_feeds() {
    FeedResults result;
    // Negative branch: no installed feed, hence both local vectors are empty.
    TradingHost empty;
    result.empty = observe([&] { (empty.*access(StageViews{}))(); });
    const auto& no_feed = Consumer::bound(empty).*access(PendingViews{});
    result.empty_transport = no_feed.descriptors.empty() && no_feed.slots.empty();

    TradingHost host;
    const FeedInput input;
    const auto bars = tape<kLong>();
    const std::array<const char*, kFeeds> keys{{"TEST:ALPHA", "TEST:BETA", "TEST:GAMMA"}};
    for (const char* key : keys)
        result.installed &= host.set_symbol_feed_borrowed(
            key, "1", input.bars.data(), input.closes.data(),
            static_cast<int>(input.bars.size()));

    result.first = stage_series<kN>(host, input);
    result.short_bars = run_source(host, bars.data(), kN);
    result.short_run = stage_series<kN>(host, input);
    result.long_bars = run_source(host, bars.data(), kLong);
    result.long_run = stage_series<kLong>(host, input);
    return result;
}
template<std::size_t Calls>
void check_feed_series(const char* row, const char* phase, const FeedSeries<Calls>& series) {
    // Each returned table has three descriptors AND three pointers into the
    // receiver's copied descriptors. This witnesses the nonempty source loop.
    ROW_CHECK(row, series.correct_transports == Calls);
    for (std::size_t i = 0; i < Calls; ++i) {
        check_release(row, phase, i, series.samples[i]);
        ROW_CHECK(row, series.samples[i].after.allocations > series.samples[i].before.allocations);
    }
}
void feed_row(const char* row) {
    // Both local buffers are used together. Reuse the same first-call receipts
    // for both named row assertions, rather than warming away the second row's
    // first-use retention check by re-running the entire fixture.
    static const FeedResults result = exercise_feeds();
    ROW_CHECK(row, result.installed && result.empty_transport);
    check_release(row, "no-feeds", 0, result.empty);
    ROW_CHECK(row, result.short_bars.returned && result.short_bars.clean
                   && result.short_bars.bars == kN && result.short_bars.trades == kN / 8);
    ROW_CHECK(row, result.long_bars.returned && result.long_bars.clean
                   && result.long_bars.bars == kLong && result.long_bars.trades == kLong / 8);
    check_feed_series(row, "first-nonempty", result.first);
    check_feed_series(row, "after-16-bars", result.short_run);
    check_feed_series(row, "after-64-bars", result.long_run);
    // Zero retained bytes and blocks after EVERY call implies the same zero
    // local contribution at 16 and 64 primary bars; no fixed host byte total
    // is assumed. The receiver was primed to the same configured shape.
    const auto& short_end = result.short_run.samples.back();
    const auto& long_end = result.long_run.samples.back();
    ROW_CHECK(row, same_live(short_end.before, short_end.after)
                   && same_live(long_end.before, long_end.after));
}

class FxHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++seen; }
    std::size_t seen = 0;
};
NativeRunSpec fx_spec(std::uint64_t run_number) {
    NativeRunSpec spec;
    spec.identity = {"view-fx", run_number};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:FX";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}
struct FxInput {
    std::array<std::int64_t, kFxPoints> timestamps{};
    std::array<double, kFxPoints> rates_a{}, rates_b{};
    NativeFxCurve seed;
    FxInput() {
        for (std::size_t i = 0; i < kFxPoints; ++i) {
            timestamps[i] = kStart + static_cast<std::int64_t>(i) * kMinute;
            rates_a[i] = 1.0 + static_cast<double>(i) / 16.0;
            rates_b[i] = rates_a[i] + 1.0;
        }
        seed.effective_from_ms.assign(timestamps.begin(), timestamps.end());
        seed.account_per_quote.assign(rates_a.begin(), rates_a.end());
    }
    pf_native_fx_curve_v1 transport(bool second) const {
        return {static_cast<std::uint32_t>(sizeof(pf_native_fx_curve_v1)),
                static_cast<std::uint32_t>(kFxPoints), timestamps.data(),
                second ? rates_b.data() : rates_a.data()};
    }
};
bool fx_matches(const FxHost& host, const FxInput& input, bool second) {
    const auto& curve = Consumer::bound(host).*access(StagedFx{});
    const auto& rates = second ? input.rates_b : input.rates_a;
    return curve && curve->effective_from_ms.size() == input.timestamps.size()
        && curve->account_per_quote.size() == rates.size()
        && std::equal(curve->effective_from_ms.begin(), curve->effective_from_ms.end(),
                      input.timestamps.begin())
        && std::equal(curve->account_per_quote.begin(), curve->account_per_quote.end(), rates.begin());
}
template<std::size_t Calls> struct FxSeries {
    std::array<Observation, Calls> samples{};
    std::size_t applied = 0, correct_transports = 0;
};
template<std::size_t Calls> FxSeries<Calls> fx_series(FxHost& host, const FxInput& input) {
    FxSeries<Calls> result;
    for (std::size_t i = 0; i < Calls; ++i) {
        // Alternate content, with the same two vector lengths. The staged
        // result must change each time, independently witnessing the C copy.
        const bool second = i % 2 == 0;
        const auto transport = input.transport(second);
        int status = -2;
        pf_native_fx_curve_error_t error = PF_NATIVE_FX_CURVE_ERROR_WRONG_PHASE;
        std::uint64_t index = std::numeric_limits<std::uint64_t>::max();
        result.samples[i] = observe([&] {
            status = strategy_configure_native_fx_curve_ext_v1(
                reinterpret_cast<pf_strategy_t>(&host), &transport, &error, &index);
        });
        if (status == 0 && error == PF_NATIVE_FX_CURVE_ERROR_NONE && index == 0) ++result.applied;
        if (fx_matches(host, input, second)) ++result.correct_transports;
    }
    return result;
}
RunEvidence run_fx(FxHost& host, const Bar* bars, std::size_t n) {
    RunEvidence result;
    host.seen = 0;
    try {
        host.run(bars, static_cast<int>(n), "1", "1");
        result.returned = true;
    } catch (...) {}
    result.clean = host.last_error().empty()
        && host.native_state().kind == NativeLifecycleKind::Completed;
    result.bars = host.seen;
    return result;
}
template<std::size_t Calls>
void check_fx_series(const char* row, const char* phase, const FxSeries<Calls>& series) {
    ROW_CHECK(row, series.applied == Calls && series.correct_transports == Calls);
    for (std::size_t i = 0; i < Calls; ++i) {
        check_release(row, phase, i, series.samples[i]);
        ROW_CHECK(row, series.samples[i].after.allocations > series.samples[i].before.allocations);
    }
}
void fx_row(const char* row) {
    FxHost host;
    const FxInput input;
    const auto bars = tape<kLong>();
    ROW_CHECK(row, host.configure_native(fx_spec(1)).status == NativeSetupStatus::Applied);
    // Negative branch precedes the first positive C copy. It cannot allocate
    // either target vector, even under the retained-static control.
    const pf_native_fx_curve_v1 empty{
        static_cast<std::uint32_t>(sizeof(pf_native_fx_curve_v1)), 0, nullptr, nullptr};
    int empty_status = -2;
    const auto empty_sample = observe([&] {
        empty_status = strategy_configure_native_fx_curve_v1(
            reinterpret_cast<pf_strategy_t>(&host), &empty);
    });
    ROW_CHECK(row, empty_status == 0 && !(Consumer::bound(host).*access(StagedFx{})));
    check_release(row, "empty-curve", 0, empty_sample);

    // Prime the retained consumer directly, never via the C transport under
    // test. Replacement then changes content, but not the persistent shape.
    ROW_CHECK(row, host.configure_native_fx_curve(input.seed).status == NativeSetupStatus::Applied);
    const auto first = fx_series<kN>(host, input);
    check_fx_series(row, "first-C-copy", first);
    const auto short_bars = run_fx(host, bars.data(), kN);
    ROW_CHECK(row, short_bars.returned && short_bars.clean && short_bars.bars == kN);
    ROW_CHECK(row, host.configure_native(fx_spec(2)).status == NativeSetupStatus::Applied);
    ROW_CHECK(row, host.configure_native_fx_curve(input.seed).status == NativeSetupStatus::Applied);
    const auto short_run = fx_series<kN>(host, input);
    check_fx_series(row, "after-16-bars", short_run);
    const auto long_bars = run_fx(host, bars.data(), kLong);
    ROW_CHECK(row, long_bars.returned && long_bars.clean && long_bars.bars == kLong);
    ROW_CHECK(row, host.configure_native(fx_spec(3)).status == NativeSetupStatus::Applied);
    ROW_CHECK(row, host.configure_native_fx_curve(input.seed).status == NativeSetupStatus::Applied);
    const auto long_run = fx_series<kLong>(host, input);
    check_fx_series(row, "after-64-bars", long_run);
    ROW_CHECK(row, same_live(short_run.samples.back().before, short_run.samples.back().after)
                   && same_live(long_run.samples.back().before, long_run.samples.back().after));
}

constexpr const char* kScratchOperation =
    "View scratch original scope diagnostic with enough text to require owned message storage";
RunFailureValue fresh_diagnostic_payload(const RunFailureValue& model) {
    // The bytes are an already validated canonical value, not a second model
    // or a new argument parser. This is the actual injected error's payload:
    // no prototype outside the cycle may keep its allocation alive. Counting
    // this shared allocation also avoids relying on a standard library's
    // choice of malloc versus operator new for system_error::what().
    return {model.code, model.args
        ? std::make_shared<const std::string>(*model.args) : nullptr};
}
class FaultBacking final : public pd::ScratchBacking {
public:
    FaultBacking(pd::FixedMemoryScratchBacking& inner, const RunFailureValue& value)
        : ScratchBacking(inner.kind(), inner.page_size()), inner_(inner),
          original_(fresh_diagnostic_payload(value), ENOSPC, kScratchOperation) {}
    bool fail_next_write = false;
    std::size_t failed_writes = 0;
    std::uint64_t page_count() const noexcept override { return inner_.page_count(); }
    const pd::ReportScratchIoError* carried_failure() const noexcept override {
        return carrying_ ? &original_ : nullptr;
    }
protected:
    Status do_read_page(pd::ScratchPageId id, pd::ScratchBuffer bytes) const noexcept override {
        carrying_ = false;
        return inner_.read_page(id, bytes);
    }
    Status do_write_page(pd::ScratchPageId id, pd::ScratchBytes bytes) noexcept override {
        if (fail_next_write) {
            fail_next_write = false; // rollback can use the backing normally
            carrying_ = true;
            ++failed_writes;
            return Status::io_error;
        }
        carrying_ = false;
        return inner_.write_page(id, bytes);
    }
    Status do_sync(pd::ScratchDurability durability) noexcept override {
        carrying_ = false;
        return inner_.sync(durability);
    }
private:
    pd::FixedMemoryScratchBacking& inner_;
    pd::ReportScratchIoError original_;
    mutable bool carrying_ = false;
};
bool original_error(const pd::ReportScratchIoError& error) {
    return error.code().value() == ENOSPC
        && error.run_failure_code() == RunFailureCode::report_sink_failed
        && std::strstr(error.what(), kScratchOperation) != nullptr;
}
struct ScratchEvidence {
    bool opened = false, inserted = false, edit_failed = false, rolled_back = false;
    bool ordinary_scope_released = false, teardown_inserted = false;
    bool teardown_failed = false, retained_one = false, first_preserved = true;
    bool next_boundary_failed = false;
    std::size_t failed_writes = 0;
};
void scratch_cycle(pd::ScratchBuffer bytes, const RunFailureValue& value,
                   ScratchEvidence& evidence) {
    // The backing bytes and validated argument model precede the window.
    // This cycle owns the injected diagnostic's message and canonical payload.
    // All source, fault-input and store owners die before the wrapper returns.
    pd::FixedMemoryScratchBacking memory(bytes, 512);
    FaultBacking backing(memory, value);
    pd::CommandScratchStore store{
        {2, 512, 1, 1, sd::exit_scratch_key_order(), pd::ScratchOpenMode::create_empty}, &backing};
    evidence.opened = store.status() == Status::ok;
    sd::ExitScratchContext context{store, {}};
    ps::PineExecutionAdapter adapter;
    adapter.bind_exit_scratch(context);
    {
        sd::ExitScratchRows<std::uint64_t> rows(adapter);
        rows.push_back(7);
        evidence.inserted = rows.size() == 1 && store.record_count() == 1 && rows.front() == 7;
        backing.fail_next_write = true;
        try { rows.push_back(11); }
        catch (const pd::ReportScratchIoError& error) { evidence.edit_failed = original_error(error); }
        catch (...) {}
        evidence.rolled_back = !store.transaction_active() && rows.size() == 1
            && store.record_count() == 1;
    }
    evidence.ordinary_scope_released = store.record_count() == 0;
    try {
        sd::ExitScratchRows<std::uint64_t> rows(adapter);
        rows.push_back(13);
        evidence.teardown_inserted = rows.size() == 1 && store.record_count() == 1;
        // The next backing write is made by real scope teardown, not a test
        // copy of remember() or the scope's erase algorithm.
        backing.fail_next_write = true;
    } catch (const pd::ReportScratchIoError& error) { evidence.teardown_failed = original_error(error); }
    catch (...) {}

    auto& retained = failure_record(context.teardown_failure, access(FailureRecord{}));
    evidence.retained_one = context.teardown_failure.pending() && retained.has_value()
        && original_error(*retained);
    // One first error -> one member record. Further remember attempts do not
    // append payloads or replace the first typed diagnostic.
    const auto remembered = counts();
    for (std::size_t i = 0; i < kLong; ++i) {
        context.teardown_failure.remember(Status::programming_error, store);
        evidence.first_preserved &= retained.has_value() && original_error(*retained)
            && same_live(remembered, counts());
    }
    try { sd::exit_scratch_check_teardown(context); }
    catch (const pd::ReportScratchIoError& error) { evidence.next_boundary_failed = original_error(error); }
    catch (...) {}
    evidence.failed_writes = backing.failed_writes;
}
struct ScratchSeries {
    std::array<Observation, kN + kLong> samples{};
    std::array<ScratchEvidence, kN + kLong> evidence{};
};
void scratch_row(const char* row) {
    std::array<std::byte, 512 * 1024> backing_bytes{};
    // Preparing canonical arguments also warms diagnostic construction without
    // calling either target function. Each FaultBacking constructs a fresh
    // diagnostic in the window; this prototype owns neither its payload nor
    // its message. Test result arrays and backing bytes never allocate there.
    const pd::ReportScratchIoError prototype(ENOSPC, kScratchOperation,
        {{"reason", "arena_exhausted"}, {"store", "command"},
         {"arena_bytes", std::int64_t{4096}}, {"high_water_bytes", std::int64_t{4096}}});
    const auto value = prototype.run_failure();
    ROW_CHECK(row, value.code == RunFailureCode::report_sink_failed && value.args);
    ScratchSeries result;
    const auto baseline = counts();
    for (std::size_t i = 0; i < result.samples.size(); ++i) {
        result.samples[i] = observe([&] {
            scratch_cycle({backing_bytes.data(), backing_bytes.size()}, value, result.evidence[i]);
        });
        const auto& evidence = result.evidence[i];
        ROW_CHECK(row, evidence.opened && evidence.inserted && evidence.edit_failed
                       && evidence.rolled_back && evidence.ordinary_scope_released);
        ROW_CHECK(row, evidence.teardown_inserted && evidence.teardown_failed
                       && evidence.retained_one && evidence.first_preserved
                       && evidence.next_boundary_failed && evidence.failed_writes == 2);
        check_release(row, i < kN ? "16-cycles" : "64-more-cycles", i, result.samples[i]);
        // Keep the ORIGINAL baseline. Re-basing after a first leaked optional
        // would allow retained static storage to look steady thereafter.
        ROW_CHECK(row, result.samples[i].after.bytes == baseline.bytes);
        ROW_CHECK(row, result.samples[i].after.blocks == baseline.blocks);
        ROW_CHECK(row, result.samples[i].after.allocations > result.samples[i].before.allocations);
    }
    ROW_CHECK(row, same_live(result.samples[kN - 1].after, baseline)
                   && same_live(result.samples.back().after, baseline)
                   && same_live(result.samples[kN - 1].after, result.samples.back().after));
}

struct Row {
    const char* id;
    void (*prove)(const char*);
    const char* listed;
};
} // namespace

int main() {
    view_allocation::test_thread = true;
    const Row rows[] = {
        {"placementaudit_hits", nullptr, "PlacementAudit is an anonymous-namespace .cpp class; default audit flag is off"},
        {"recordedseries_open_ms", nullptr, "RecordedSeries::open_ms vector is absent; current RecordedSeriesRow::open_ms is scalar"},
        {"recordedseries_values", nullptr, "RecordedSeries::values vector is absent; current RecordedSeriesRow::value is scalar"},
        {"pinestrategyhost_adapter", nullptr, "embedded adapter root aggregates separate constituent populations"},
        {"pinestrategyhost_scheduler", nullptr, "embedded scheduler root aggregates separate constituent populations"},
        {"exitscratchteardownfailure_failure_and_scope_error_optionals", scratch_row, nullptr},
        {"fixed_count_imported_native_identity_context_objects_in_source_members", nullptr, "fixed imported slots are not a container population; intern ownership has a separate row"},
        {"sessionkey_intern_pool", nullptr, "intern pool is an inaccessible function-local static and intentionally retains distinct texts"},
        {"pinestrategyhost_stage_symbol_feed_views_descriptors", feed_row, nullptr},
        {"pinestrategyhost_stage_symbol_feed_views_slots", feed_row, nullptr},
        {"nativefxcurve_effective_from_ms_and_account_per_quote", fx_row, nullptr},
    };
    for (const auto& row : rows) {
        if (!row.prove) {
            continue;
        }
        try { row.prove(row.id); }
        catch (const std::exception& error) {
            check(row.id, false, "unexpected row exception", __LINE__);
            std::fprintf(stderr, "DETAIL [%s] %s\n", row.id, error.what());
        } catch (...) { check(row.id, false, "unexpected non-standard row exception", __LINE__); }
    }
    std::printf("memory proof release_views_scratch: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
