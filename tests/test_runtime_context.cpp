// #310 fix acceptance. WRITTEN NOT RUN by the source author.
// Generic host/evaluator controls; no Pine source or selected timeframe list.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include <pineforge/runtime_context.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_ta_length.hpp>

#include "../src/source/multi_day_periods.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace pineforge;
int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); ++failures; \
} } while (false)

template<class Error, class F> bool throws(F&& body) {
    try { body(); } catch (const Error&) { return true; }
    catch (...) { return false; }
    return false;
}

void check_budget_failure(const RuntimeHistoryBudgetExhausted& error,
                          std::uint64_t configured, std::uint64_t used,
                          std::uint64_t needed) {
    CHECK(error.configured == configured && error.used == used && error.needed == needed);
    const std::runtime_error& base = error;
    const auto* info = dynamic_cast<const RunFailureInfo*>(&base);
    CHECK(info != nullptr);
    const auto value = classify_run_failure(base);
    CHECK(value.code == RunFailureCode::runtime_history_budget_exhausted);
    CHECK(info && info->run_failure_code() == value.code);
    CHECK(run_failure_code_class(value.code) == RunFailureClass::resource);
    CHECK(!run_failure_code_retryable(value.code));
    const auto expected = "{\"configured\":\"" + std::to_string(configured)
        + "\",\"needed\":\"" + std::to_string(needed)
        + "\",\"used\":\"" + std::to_string(used) + "\"}";
    CHECK(value.args && *value.args == expected);
    CHECK(std::string(error.what()) == "runtime_history_budget_exhausted: configured="
        + std::to_string(configured) + " used=" + std::to_string(used)
        + " needed=" + std::to_string(needed));
}

void check_observation_failure(const RuntimeStorageObservationIncomplete& error,
                               const OwnedStorageUsage& expected) {
    CHECK(!error.observation.complete());
    CHECK(error.observation.storage_bytes() == std::numeric_limits<std::uint64_t>::max());
    CHECK(error.observation.resident_metadata_bytes == expected.resident_metadata_bytes);
    CHECK(error.observation.resident_payload_bytes == expected.resident_payload_bytes);
    CHECK(error.observation.backing_bytes == expected.backing_bytes);
    CHECK(error.observation.unsupported == expected.unsupported);
    CHECK(error.observation.overflow == expected.overflow);
    const std::runtime_error& base = error;
    const auto* info = dynamic_cast<const RunFailureInfo*>(&base);
    CHECK(info != nullptr);
    const auto value = classify_run_failure(base);
    CHECK(value.code == RunFailureCode::runtime_storage_observation_incomplete);
    CHECK(info && info->run_failure_code() == value.code);
    CHECK(run_failure_code_class(value.code) == RunFailureClass::unsupported);
    CHECK(!run_failure_code_retryable(value.code));
    const auto json = "{\"backing_bytes_lower_bound\":\"" + std::to_string(expected.backing_bytes)
        + "\",\"overflow\":" + (expected.overflow ? "true" : "false")
        + ",\"resident_metadata_bytes_lower_bound\":\""
        + std::to_string(expected.resident_metadata_bytes)
        + "\",\"resident_payload_bytes_lower_bound\":\""
        + std::to_string(expected.resident_payload_bytes)
        + "\",\"unsupported_flags\":\"" + std::to_string(expected.unsupported) + "\"}";
    CHECK(value.args && *value.args == json);
    CHECK(std::string(error.what()) == "runtime context owned storage observation is incomplete");
}

Bar bar(std::uint64_t i) {
    const double price = static_cast<double>(i + 1);
    return {price, price + 2, price - 1, price, 10, static_cast<std::int64_t>(i) * 60000};
}

struct Sum : RuntimeContextEvaluator {
    double total = 0;
    std::uint64_t calls = 0;
    std::vector<unsigned char> payload;
    explicit Sum(std::size_t bytes = 0) : payload(bytes) {}
    void consume(RuntimeContextService&, RuntimeContextHandle,
                 const RuntimeContextEvent& event) override {
        CHECK(event.frontier == event.index + 1);
        CHECK(event.index == calls);
        total += event.bar.close;
        ++calls;
    }
    std::uint64_t storage_bytes() const noexcept override {
        OwnedStorage out;
        observe_owned_storage(out);
        return out.usage().storage_bytes();
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept override {
        out.metadata(sizeof(*this));
        pineforge::observe_owned_storage(out, payload);
    }
};
RuntimeContextService::Factory sum_factory(std::size_t bytes = 0) {
    return [bytes] { return std::make_unique<Sum>(bytes); };
}
Sum& sum(RuntimeContextService& service, RuntimeContextHandle h) {
    return dynamic_cast<Sum&>(service.published(h));
}

void batch_lease_replay_release_and_generation() {
    RuntimeContextService service;
    std::array<Bar, 7> bars;
    for (std::size_t i = 0; i < bars.size(); ++i) bars[i] = bar(i);
    RuntimeContextHandle expired;
    {
        RuntimeBatchLease lease(bars.data(), bars.size());
        service.bind_batch(lease);
        service.accept_batch(4);
        const auto h = service.get_or_create({21, "semantic value", {}}, sum_factory());
        CHECK(throws<std::logic_error>([&] { service.published(h); }));
        service.advance(h, 3);
        CHECK(sum(service, h).calls == 3 && sum(service, h).total == 6);
        service.advance(h, 3);
        CHECK(sum(service, h).calls == 3); // no duplicate replay
        CHECK(service.frontier_before(60000) == 1);
        CHECK(service.frontier_before(180000) == 3);
        CHECK(throws<std::out_of_range>([&] { service.advance(h, 5); }));
        service.advance(h, 4);
        CHECK(sum(service, h).total == 10); // frontier+1 mutation must fail
        service.accept_batch(7);
        service.advance(h, 7);
        CHECK(sum(service, h).calls == 7 && sum(service, h).total == 28);
        CHECK(service.get_or_create({21, "semantic value", {}}, sum_factory()) == h);
        service.release(h);
        CHECK(throws<std::logic_error>([&] { service.published(h); }));
        expired = service.get_or_create({21, "semantic value", {}}, sum_factory());
        CHECK(expired != h);
    }
    CHECK(throws<std::logic_error>([&] { service.advance(expired, 7); }));
    CHECK(throws<std::logic_error>([&] { service.published(expired); }));
    service.reset();
    RuntimeBatchLease next(bars.data(), bars.size());
    service.bind_batch(next);
    service.accept_batch(2);
    const auto fresh = service.get_or_create({21, "semantic value", {}}, sum_factory());
    CHECK(fresh.generation != expired.generation);
    CHECK(throws<std::logic_error>([&] { service.advance(expired, 2); }));
    service.advance(fresh, 2);
    CHECK(sum(service, fresh).total == 3 && sum(service, fresh).calls == 2);
    auto wrong_generation = fresh;
    wrong_generation.generation = expired.generation;
    CHECK(throws<std::logic_error>([&] { service.published(wrong_generation); }));

    RuntimeContextService other;
    RuntimeBatchLease other_lease(bars.data(), bars.size());
    other.bind_batch(other_lease);
    other.accept_batch(2);
    const auto other_handle = other.get_or_create({21, "semantic value", {}}, sum_factory());
    other.advance(other_handle, 2);
    CHECK(throws<std::logic_error>([&] { other.published(fresh); }));
}

// An owning, seekable caller source. No service-owned history copy is needed.
struct Source : RuntimeHistorySource {
    std::uint64_t count;
    std::shared_ptr<bool> destroyed;
    Source(std::uint64_t n, std::shared_ptr<bool> flag) : count(n), destroyed(std::move(flag)) {}
    ~Source() override { *destroyed = true; }
    std::uint64_t size() const override { return count; }
    Bar read(std::uint64_t i) const override {
        if (i >= count) throw std::out_of_range("source");
        return bar(i);
    }
};
void owning_source_survives_entries_and_releases() {
    RuntimeContextService service;
    auto destroyed = std::make_shared<bool>(false);
    auto source = std::make_shared<Source>(513, destroyed);
    service.bind_source(source);
    source.reset();
    CHECK(!*destroyed);
    service.accept_batch(131);
    const auto h = service.get_or_create({7, "first entry", {}}, sum_factory());
    service.advance(h, 131);
    service.accept_batch(513);
    service.advance(h, 513);
    CHECK(sum(service, h).total == 513.0 * 514 / 2);
    service.end_run();
    CHECK(*destroyed);
    CHECK(throws<std::logic_error>([&] { service.published(h); }));
}

void batch_storage_is_independent_of_caller_history_size() {
    std::uint64_t resident = 0, accounted = 0;
    for (const std::size_t n : {3u, 73u, 513u}) {
        std::vector<Bar> caller_bars;
        for (std::size_t i = 0; i < n; ++i) caller_bars.push_back(bar(i));
        RuntimeBatchLease lease(caller_bars.data(), caller_bars.size());
        RuntimeContextService service;
        service.bind_batch(lease);
        service.accept_batch(n);
        const auto h = service.get_or_create({6, "leased source", {}}, sum_factory());
        service.advance(h, n);
        CHECK(service.usage().spool_bytes == 0);
        CHECK(sum(service, h).total == n * (n + 1.0) / 2);
        if (!resident) {
            resident = service.usage().resident_bytes;
            accounted = service.usage().accounted_bytes;
        } else {
            CHECK(service.usage().resident_bytes == resident);
            CHECK(service.usage().accounted_bytes == accounted);
        }
    }
}

void factory_cycles_do_not_publish_a_partial_context() {
    RuntimeContextService service;
    std::array<Bar, 1> bars{bar(0)};
    RuntimeBatchLease lease(bars.data(), bars.size());
    service.bind_batch(lease);
    service.accept_batch(1);
    const RuntimeContextKey key{13, "factory", {}};
    CHECK(throws<std::logic_error>([&] {
        service.get_or_create(key, [&]() -> std::unique_ptr<RuntimeContextEvaluator> {
            CHECK(throws<std::logic_error>([&] { service.reset(); }));
            service.get_or_create(key, sum_factory());
            return std::make_unique<Sum>();
        });
    }));
    CHECK(service.usage().contexts == 0);
    const auto h = service.get_or_create(key, sum_factory());
    service.advance(h, 1);
    CHECK(sum(service, h).total == 1);
}

struct Nested : RuntimeContextEvaluator {
    RuntimeContextHandle child;
    double last = 0;
    std::uint64_t calls = 0;
    void consume(RuntimeContextService& service, RuntimeContextHandle self,
                 const RuntimeContextEvent& event) override {
        CHECK(service.active_context() == self);
        CHECK(service.callback_frontier() == event.frontier);
        CHECK(throws<std::logic_error>([&] { service.published(self); }));
        CHECK(throws<std::logic_error>([&] { service.release(self); }));
        CHECK(throws<std::logic_error>([&] { service.reset(); }));
        child = service.get_or_create({90, "child args", self}, sum_factory());
        CHECK(throws<std::out_of_range>([&] {
            service.advance(child, service.accepted_frontier() + 1);
        }));
        service.advance(child, event.frontier);
        last = sum(service, child).total;
        CHECK(sum(service, child).calls == event.frontier);
        CHECK(service.active_context() == self); // nested dispatch restored it
        ++calls;
    }
    std::uint64_t storage_bytes() const noexcept override { return sizeof(*this); }
    void observe_owned_storage(OwnedStorage& out) const noexcept override {
        out.metadata(sizeof(*this)); // handle/counters only; child is a service root
    }
};
void nested_discovery_keeps_references_and_keys_separate() {
    RuntimeContextService service;
    std::array<Bar, 9> bars;
    for (std::size_t i = 0; i < bars.size(); ++i) bars[i] = bar(i);
    RuntimeBatchLease lease(bars.data(), bars.size());
    service.bind_batch(lease);
    service.accept_batch(bars.size());
    const auto factory = [] { return std::make_unique<Nested>(); };
    const auto a = service.get_or_create({1, "parent a", {}}, factory);
    const auto b = service.get_or_create({1, "parent b", {}}, factory);
    service.advance(a, 9);
    auto& first = dynamic_cast<Nested&>(service.published(a));
    const auto* address = &first;
    service.advance(b, 4);
    auto& second = dynamic_cast<Nested&>(service.published(b));
    CHECK(first.child != second.child && first.last == 45 && second.last == 10);
    CHECK(&dynamic_cast<Nested&>(service.published(a)) == address);
    CHECK(throws<std::logic_error>([&] { service.release(a); }));
    service.release(first.child);
    service.release(a);
    service.advance(b, 9);
    CHECK(second.last == 45 && second.calls == 9);
    CHECK(service.usage().contexts == 2);
}

void seed_live(RuntimeContextService& service, std::size_t n,
               std::optional<std::uint64_t> budget, std::size_t resident = 0) {
    service.configure({resident, budget});
    service.enable_history();
    service.begin_live();
    for (std::size_t i = 0; i < n; ++i) service.append_live(bar(i));
}
void exact_budget_and_default_none() {
    // Three independently recreated histories. B is actual service accounting
    // after an allocated payload; the refused state requests one byte more.
    for (const std::size_t n : {3u, 73u, 513u}) {
        const auto bytes = 3 * n + 11;
        RuntimeContextService unlimited;
        seed_live(unlimited, n, std::nullopt);
        const auto h = unlimited.get_or_create({4, "budgeted context", {}}, sum_factory(bytes));
        unlimited.advance(h, n);
        CHECK(sum(unlimited, h).total == n * (n + 1.0) / 2);
        const auto b = unlimited.usage().accounted_bytes;
        CHECK(unlimited.usage().spool_bytes == n * sizeof(Bar));

        RuntimeContextService exact;
        seed_live(exact, n, b);
        const auto fits = exact.get_or_create({4, "budgeted context", {}}, sum_factory(bytes));
        exact.advance(fits, n);
        CHECK(exact.usage().accounted_bytes == b);
        CHECK(sum(exact, fits).calls == n);

        RuntimeContextService over;
        seed_live(over, n, b);
        const auto before = over.usage();
        try {
            over.get_or_create({4, "budgeted context", {}}, sum_factory(bytes + 1));
            CHECK(false);
        } catch (const RuntimeHistoryBudgetExhausted& error) {
            CHECK(error.configured == b);
            CHECK(error.used == before.accounted_bytes);
            CHECK(error.used + error.needed == b + 1);
            CHECK(std::string(error.what()).find("runtime_history_budget_exhausted") != std::string::npos);
            check_budget_failure(error, b, before.accounted_bytes, b + 1 - before.accounted_bytes);
        }
        CHECK(over.usage().contexts == 0);
        CHECK(over.usage().accepted == n);
        // A former 64 MiB refusal default would fail this reached allocation.
        // The resident spill threshold does not impose that budget.
        const auto large = unlimited.get_or_create(
            {5, "no implicit budget", {}}, sum_factory(64u * 1024u * 1024u + 1u));
        unlimited.advance(large, n);
        CHECK(sum(unlimited, large).calls == n);
    }
    RuntimeContextService zero;
    seed_live(zero, 7, 0);
    // Reached dynamic contexts alone consult the budget, not an enable flag
    // or unreached template. There is no budget refusal before first reach.
    CHECK(throws<RuntimeHistoryBudgetExhausted>([&] {
        zero.get_or_create({8, "reached", {}}, sum_factory());
    }));
}

void spill_and_fixed_resident_bookkeeping() {
    for (const std::size_t resident : std::array<std::size_t, 2>{0, 2 * sizeof(Bar)}) {
        RuntimeContextService service;
        seed_live(service, 3, std::nullopt, resident);
        const auto h = service.get_or_create({3, "fixed inventory", {}}, sum_factory());
        service.advance(h, 3);
        const auto baseline = service.usage().resident_bytes;
        for (const std::size_t end : {73u, 513u, 2051u}) {
            for (auto i = service.accepted_frontier(); i < end; ++i)
                service.append_live(bar(i));
            service.advance(h, end);
            CHECK(service.usage().resident_bytes == baseline);
            CHECK(service.usage().spool_bytes == end * sizeof(Bar));
            CHECK(sum(service, h).total == end * (end + 1.0) / 2);
        }
    }
    // These are exact bookkeeping checks, not the owner's RSS/PSS proof.
    // Its literal harness/tolerance is a required supervisor input.
}

class HandwrittenHost : public source::PineStrategyHost {
public:
    bool dynamic = true;
    std::uint64_t bars_seen = 0;
    double last = 0;
    RuntimeContextHandle handle{};
    void configure_security_evaluators() override {
        if (dynamic) runtime_contexts().enable_history();
    }
    void on_source_bar(const Bar&) override {
        ++bars_seen;
        if (!dynamic) return;
        auto& service = runtime_contexts();
        handle = service.get_or_create({77, "host callback", {}}, sum_factory());
        service.advance(handle, runtime_security_frontier());
        last = sum(service, handle).total;
    }
};
void source_context_metadata_restores_across_nested_callbacks() {
    HandwrittenHost host, peer;
    source::PineRuntimeSecurityCursor parent, child;
    parent.timeframe = "60";
    child.timeframe = "120";
    CHECK(source::PineRuntimeSecurityScope::current(host) == nullptr);
    {
        source::PineRuntimeSecurityScope outer(host, parent);
        CHECK(host.runtime_security_timeframe("") == "60");
        CHECK(host.runtime_security_timeframe("15") == "15");
        CHECK(source::PineRuntimeSecurityScope::current(peer) == nullptr);
        try {
            source::PineRuntimeSecurityScope inner(host, child);
            CHECK(host.runtime_security_timeframe("") == "120");
            throw std::runtime_error("unwind nested callback");
        } catch (const std::runtime_error&) {}
        CHECK(host.runtime_security_timeframe("") == "60");
    }
    CHECK(source::PineRuntimeSecurityScope::current(host) == nullptr);
}
void native_host_batch_stream_and_static_budget_controls() {
    std::array<Bar, 9> bars;
    for (std::size_t i = 0; i < bars.size(); ++i) bars[i] = bar(i);
    HandwrittenHost host;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK(host.last_error().empty());
    CHECK(host.bars_seen == 9 && host.last == 45);
    const auto old = host.handle;
    CHECK(throws<std::logic_error>([&] { host.runtime_contexts().published(old); }));
    host.bars_seen = 0; host.last = 0;
    host.run(bars.data(), 4, "1", "1");
    CHECK(host.last_error().empty());
    CHECK(host.bars_seen == 4 && host.last == 10 && host.handle != old);

    // Borrowed warmup dies between stream entries; replay uses owned spill.
    HandwrittenHost live;
    live.runtime_contexts().configure({0, std::nullopt});
    {
        std::vector<Bar> warmup(bars.begin(), bars.begin() + 3);
        CHECK(live.stream_begin(warmup.data(), 3, "1", "1"));
    }
    for (std::size_t i = 3; i < bars.size(); ++i) CHECK(live.stream_push_bar(bars[i]));
    CHECK(live.last == 45 && live.bars_seen == 9);
    CHECK(live.stream_end());

    HandwrittenHost ticks;
    ticks.runtime_contexts().configure({0, std::nullopt});
    CHECK(ticks.stream_begin(bars.data(), 3, "1", "1"));
    for (std::size_t i = 3; i < bars.size(); ++i)
        CHECK(ticks.stream_push_tick({bars[i].timestamp, i + 1, bars[i].close, 1.0}));
    CHECK(ticks.stream_end(true));
    CHECK(ticks.last == 45 && ticks.bars_seen == 9);

    for (const auto budget : std::array<std::optional<std::uint64_t>, 3>{
            std::nullopt, 0, 1}) {
        HandwrittenHost control;
        control.dynamic = false;
        control.runtime_contexts().configure({0, budget});
        control.run(bars.data(), 9, "1", "1");
        CHECK(control.last_error().empty() && control.bars_seen == 9);
        CHECK(control.runtime_contexts().usage().accepted == 0);
    }
}

// These controls compare public observations with the independent global
// new/delete census, including STL rebinds, shared-control allocations and
// accounting's own retained ledgers. No private node/block geometry is pinned.
// The observer's temporary identity vector is destroyed before each comparison.
struct AllocationCensus {
    AllocationCensus() {
        CHECK(!global_allocation::recording && global_allocation::live_bytes == 0);
        global_allocation::recording = true;
    }
    ~AllocationCensus() {
        CHECK(global_allocation::live_bytes == 0);
        global_allocation::recording = false;
    }
};
template<class... T>
OwnedStorageUsage external_storage(const T&... values) {
    OwnedStorage out;
    observe_owned_members(out, values...);
    return out.usage();
}
template<class... T>
void check_external_census(const T&... values) {
    const auto observed = external_storage(values...);
    CHECK(observed.complete());
    CHECK(observed.resident_bytes() == global_allocation::live_bytes);
    CHECK(observed.backing_bytes == 0);
}

bool same_bar(const Bar& a, const Bar& b) {
    return a.timestamp == b.timestamp && a.open == b.open && a.high == b.high
        && a.low == b.low && a.close == b.close && a.volume == b.volume;
}
void check_same_storage(const OwnedStorageUsage& a, const OwnedStorageUsage& b) {
    CHECK(a.complete() && b.complete());
    CHECK(a.resident_metadata_bytes == b.resident_metadata_bytes);
    CHECK(a.resident_payload_bytes == b.resident_payload_bytes);
    CHECK(a.backing_bytes == b.backing_bytes);
    CHECK(a.logical_payload_bytes == b.logical_payload_bytes);
}
void warm_symbol_clock(int64_t first, const std::string& tz, const std::string& session) {
    // timeframe.cpp's existing thread-local day-offset cache owns its own
    // timezone strings. Warm all its day slots outside the per-owner census,
    // so that a new cache entry is not attributed to an aggregator allocation.
    for (int i = 0; i < 16; ++i)
        (void)session_period_close_ms(first + i * kMsPerDay, tz, session, CalendarPeriod::DAY);
}

void aggregator_native_capacity_copy_and_release_census() {
    const std::string tz = "America/Argentina/Buenos_Aires";
    const std::string session = "0900-1100,1200-1400,1500-1600:23456";
    const auto first = native_calendar::native_civil_days(2025, 1, 1) * kMsPerDay;
    // Calendar setup is outside the census; only retained owned requests
    // belong to the runtime roots checked below.
    warm_symbol_clock(first, tz, session);
    for (const std::size_t count : {3u, 29u, 113u}) {
        AllocationCensus census;
        {
            TimeframeAggregator calendar("D", "1", tz, session);
            TimeframeAggregator ratio("60", "1", tz, session);
            std::vector<int64_t> stamps, trade, late;
            stamps.reserve(3 * count + 7); // unused capacity must be charged
            trade.reserve(2 * count + 1);  // temporary input, not a retained owner
            late.reserve(4 * count + 9);
            for (std::size_t i = 0; i < count; ++i) {
                const auto ms = first + static_cast<int64_t>(i) * kMsPerDay;
                stamps.push_back(ms);
                trade.push_back(ms + 60000);
                late.push_back(ms + 3600000);
            }
            calendar.set_native_periods(std::move(stamps), std::move(trade), CalendarPeriod::DAY);
            ratio.set_native_day_opens(std::move(late));
            check_external_census(calendar, ratio, stamps, trade, late);
            CHECK(calendar.native_period_stamps_held() == 2 * count);

            {
                auto copy = calendar;
                check_external_census(calendar, ratio, stamps, trade, late, copy);
                auto moved = std::move(copy);
                TimeframeAggregator assigned;
                assigned = moved;
                check_external_census(calendar, ratio, stamps, trade, late, copy, moved, assigned);
                assigned = std::move(moved);
                check_external_census(calendar, ratio, stamps, trade, late, copy, moved, assigned);
                CHECK(assigned.bucket_open_ms(first + 1) == calendar.bucket_open_ms(first + 1));
                CHECK(assigned.bar_label_ms(first + 1) == calendar.bar_label_ms(first + 1));
                auto input = bar(17);
                input.timestamp = first + 1;
                const auto expected = calendar.feed(input, first + kMsPerDay);
                const auto actual = assigned.feed(input, first + kMsPerDay);
                CHECK(same_bar(expected.bar, actual.bar));
                CHECK(expected.sub_bar_count == actual.sub_bar_count);
                CHECK(expected.is_complete == actual.is_complete);
                CHECK(same_bar(calendar.current(), assigned.current()));
                CHECK(same_bar(calendar.last_completed(), assigned.last_completed()));
                check_external_census(calendar, ratio, stamps, trade, late, copy, moved, assigned);
            }
            check_external_census(calendar, ratio, stamps, trade, late);
            const auto calendar_held = external_storage(calendar);
            const auto ratio_held = external_storage(ratio);
            // Both rejected replacements clear logical contents while keeping
            // the existing buffers. A stamps_held/size observer fails here.
            calendar.set_native_periods({}, {}, CalendarPeriod::DAY);
            ratio.set_native_day_opens({2, 1});
            CHECK(calendar.native_period_stamps_held() == 0);
            CHECK(!calendar.has_native_periods() && !ratio.has_native_periods());
            check_same_storage(calendar_held, external_storage(calendar));
            check_same_storage(ratio_held, external_storage(ratio));
            check_external_census(calendar, ratio, stamps, trade, late);
        }
    }
}

void multi_day_shared_params_and_sources_census() {
    namespace md = source::multi_day_detail;
    const std::string tz = "America/Argentina/Buenos_Aires";
    const std::string session = "0900-1100,1200-1400,1500-1600:23456";
    const auto parsed = native_calendar::parse_session(session, tz);
    CHECK(parsed.has_value());
    if (!parsed) return;
    std::array<Bar, 4> borrowed;
    const auto first = native_calendar::native_civil_days(2025, 1, 1) * kMsPerDay;
    for (std::size_t i = 0; i < borrowed.size(); ++i) {
        borrowed[i] = bar(i);
        borrowed[i].timestamp = first + static_cast<int64_t>(i) * kMsPerDay;
    }
    warm_symbol_clock(first, tz, session);
    for (const std::size_t capacity : {67u, 131u, 521u}) {
        AllocationCensus census;
        {
            auto params = md::make_owned<md::Params>();
            params->tz = tz;
            params->tz.reserve(capacity);
            params->session = session;
            params->session.reserve(2 * capacity);
            params->calendar = *parsed;
            params->chart = borrowed.data();
            params->n_chart = borrowed.size();
            params->aux = borrowed.data();
            params->n_aux = borrowed.size();
            // Distinct sources own distinct recent buffers/control allocations,
            // but all three share the same Params allocation and calendar data.
            auto a = md::make_owned<md::Source>(params, first, borrowed.back().timestamp);
            auto b = md::make_owned<md::Source>(params, first, borrowed.back().timestamp);
            auto c = md::make_owned<md::RescannedSource>(params, first, borrowed.back().timestamp);
            TimeframeAggregator one("D", "1", tz, session);
            TimeframeAggregator two("D", "1", tz, session);
            TimeframeAggregator three("D", "1", tz, session);
            CHECK(one.set_native_period_source(a));
            CHECK(two.set_native_period_source(b));
            CHECK(three.set_native_period_source(c));
            CHECK(a->stamps_held() == 0 && b->stamps_held() == 0 && c->stamps_held() == 0);
            check_external_census(one, two, three);
            const auto joined = external_storage(one, two, three);
            CHECK(joined.resident_bytes() < external_storage(one).resident_bytes()
                  + external_storage(two).resident_bytes() + external_storage(three).resident_bytes());
            check_same_storage(joined, external_storage(three, one, two));
            check_same_storage(joined, external_storage(*params, *b, three, one, two, one));
            CHECK(a->stamps_held() == 0 && b->stamps_held() == 0 && c->stamps_held() == 0);
            a.reset();
            b.reset();
            c.reset();
            params.reset(); // only aggregators now retain the sources/Params
            check_external_census(one, two, three);
            {
                auto copy = one;
                auto moved = std::move(copy);
                check_external_census(one, two, three, copy, moved);
                CHECK(!one.set_native_period_source(nullptr));
                // The copied aggregator still owns the original source.
                check_external_census(one, two, three, copy, moved);
                CHECK(!moved.set_native_period_source(nullptr));
                check_external_census(one, two, three, copy, moved);
            }
            CHECK(!two.set_native_period_source(nullptr));
            check_external_census(one, two, three);
            CHECK(!three.set_native_period_source(nullptr)); // final Params release
            check_external_census(one, two, three);
        }
    }
}

// Closed-form UTC daily opens are independent of the generator's walk/restart
// algorithm. Caller-owned input and reference tables live outside the census.
std::vector<int64_t> utc_period_opens(int year, std::size_t input_days, int days) {
    const auto begin = native_calendar::native_civil_days(year, 1, 1);
    const auto last_year = native_calendar::native_civil_date(
        begin + static_cast<int64_t>(input_days) - 1).year + 1;
    std::vector<int64_t> result;
    for (auto y = static_cast<int64_t>(year); y <= last_year; ++y) {
        const auto start = native_calendar::native_civil_days(y, 1, 1);
        const auto end = native_calendar::native_civil_days(y + 1, 1, 1);
        for (auto d = start; d < end; d += days) result.push_back(d * kMsPerDay);
    }
    return result;
}

void multi_day_generator_answers_copy_restart_and_release_census() {
    namespace md = source::multi_day_detail;
    NativeDayPartitionScope nominal(nullptr);
    for (const int days : {2, 5, 13}) {
        std::optional<std::size_t> cold_requests, warm_requests;
        for (const std::size_t input_days : {43u, 277u, 803u}) {
            const auto opens = utc_period_opens(2024, input_days, days);
            std::vector<Bar> chart, aux;
            const auto begin = native_calendar::native_civil_days(2024, 1, 1) * kMsPerDay;
            for (std::size_t i = 0; i < input_days; ++i) {
                auto value = bar(i);
                value.timestamp = begin + static_cast<int64_t>(i) * kMsPerDay;
                chart.push_back(value);
                value.timestamp += 3600000;
                aux.push_back(value);
            }
            source::MultiDayFeeds feeds;
            feeds.days = days;
            feeds.chart = chart.data();
            feeds.n_chart = static_cast<int>(chart.size());
            feeds.daily_chart = true;
            feeds.aux = aux.data();
            feeds.n_aux = aux.size();
            feeds.tz = "UTC";
            feeds.session = "24x7";
            const auto parsed = native_calendar::parse_session(feeds.session, feeds.tz);
            CHECK(parsed.has_value());
            if (!parsed) continue;
            TimeframeAggregator reference("D", "1");
            reference.set_native_periods(opens, opens, CalendarPeriod::DAY);
            // Resolve the same calendar paths before measuring retained owners.
            {
                auto warm = source::make_multi_day_period_source(feeds);
                CHECK(warm != nullptr);
                if (!warm) continue;
                int64_t stamp = 0;
                bool last = false;
                CHECK(warm->stamp_at(opens.back(), stamp, last));
            }
            AllocationCensus census;
            {
                auto ordered = source::make_multi_day_period_source(feeds);
                CHECK(ordered != nullptr);
                if (!ordered) continue;
                CHECK(dynamic_cast<md::Source*>(ordered.get()) != nullptr);
                auto params = md::make_owned<md::Params>();
                params->days = days;
                params->every_day = true;
                params->daily_chart = true;
                params->chart = chart.data();
                params->n_chart = chart.size();
                params->aux = aux.data();
                params->n_aux = aux.size();
                params->tz = feeds.tz;
                params->session = feeds.session;
                params->calendar = *parsed;
                // Exercise the actual fallback planner/generator against the
                // same independent table, without requiring a clock regression.
                auto rescanned = md::make_rescanned_source(std::move(params));
                CHECK(rescanned != nullptr);
                if (!rescanned) continue;
                CHECK(dynamic_cast<md::RescannedSource*>(rescanned.get()) != nullptr);
                TimeframeAggregator one("D", "1"), two("D", "1");
                CHECK(one.set_native_period_source(ordered));
                CHECK(two.set_native_period_source(rescanned));
                CHECK(ordered->last_stamp() == opens.back());
                CHECK(rescanned->last_stamp() == opens.back());
                check_external_census(one, two);
                const auto cold = external_storage(one, two);
                if (cold_requests) CHECK(*cold_requests == global_allocation::live_bytes);
                else cold_requests = global_allocation::live_bytes;
                CHECK(ordered->stamps_held() == 0 && rescanned->stamps_held() == 0);
                // The first query allocates a real generator. Fail that
                // request, require propagation/release, then recover below.
                for (const auto& partition : {ordered, rescanned}) {
                    int64_t stamp = -1;
                    bool last = false;
                    global_allocation::fail_after = 0;
                    const bool refused = throws<std::bad_alloc>([&] {
                        (void)partition->stamp_at(opens.front(), stamp, last);
                    });
                    global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
                    CHECK(refused);
                    CHECK(partition->stamps_held() == 0);
                    check_external_census(one, two);
                    check_same_storage(cold, external_storage(one, two));
                }

                const auto question = [&](int64_t ms) {
                    const auto after = std::upper_bound(opens.begin(), opens.end(), ms);
                    for (const auto& partition : {ordered, rescanned}) {
                        int64_t stamp = -1;
                        bool last = false;
                        CHECK(partition->stamp_at(ms, stamp, last) == (after != opens.begin()));
                        if (after != opens.begin()) {
                            CHECK(stamp == *(after - 1));
                            CHECK(last == (after == opens.end()));
                        }
                    }
                    CHECK(one.bucket_open_ms(ms) == reference.bucket_open_ms(ms));
                    CHECK(two.bucket_open_ms(ms) == reference.bucket_open_ms(ms));
                    CHECK(one.bar_label_ms(ms) == reference.bar_label_ms(ms));
                    CHECK(two.bar_label_ms(ms) == reference.bar_label_ms(ms));
                    CHECK(one.period_changes(ms - 1, ms) == reference.period_changes(ms - 1, ms));
                    CHECK(two.period_changes(ms - 1, ms) == reference.period_changes(ms - 1, ms));
                    CHECK(ordered->stamps_held() <= source::kMultiDayWindow);
                    CHECK(rescanned->stamps_held() <= source::kMultiDayWindow);
                };
                question(opens.front() - 1);
                for (const auto ms : opens) {
                    question(ms - 1);
                    question(ms);
                    question(ms + 1);
                }
                question(opens.back() + 2 * kMsPerDay); // nominal last-bound fallback
                check_external_census(one, two);
                const auto warm = external_storage(one, two);
                if (warm_requests) CHECK(*warm_requests == global_allocation::live_bytes);
                else warm_requests = global_allocation::live_bytes;
                CHECK(warm.resident_bytes() > cold.resident_bytes()); // actual unique generators
                CHECK(warm.resident_payload_bytes == cold.resident_payload_bytes);
                // The source is at the end now, so the first open requires
                // restart. RescannedSource releases its old generator before
                // this allocation; Source keeps its old one if allocation
                // fails. Observe the actual surviving owners in either case.
                for (const auto& partition : {ordered, rescanned}) {
                    int64_t stamp = -1;
                    bool last = false;
                    global_allocation::fail_after = 0;
                    const bool refused = throws<std::bad_alloc>([&] {
                        (void)partition->stamp_at(opens.front(), stamp, last);
                    });
                    global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
                    CHECK(refused);
                    check_external_census(one, two);
                }
                question(opens.front()); // recover from both real failures
                // Reverse and permuted questions force repeated generator
                // replacement without accumulating old generators/windows.
                for (std::size_t i = opens.size(); i > 0; --i) question(opens[i - 1]);
                check_same_storage(warm, external_storage(one, two));
                check_external_census(one, two);
                for (std::size_t i = 0; i < opens.size(); ++i)
                    question(opens[(i * 37 + 11) % opens.size()]);
                check_external_census(one, two);

                {
                    auto copy = one;
                    auto moved = std::move(copy);
                    CHECK(moved.bucket_open_ms(chart.front().timestamp)
                          == reference.bucket_open_ms(chart.front().timestamp));
                    check_external_census(one, two, copy, moved);
                    check_same_storage(external_storage(one, two), external_storage(two, one, moved));
                }
                for (std::size_t i = 0; i < chart.size(); ++i) {
                    const auto next = i + 1 < chart.size() ? chart[i + 1].timestamp : 0;
                    const auto expected = reference.feed(chart[i], next);
                    const auto a = one.feed(chart[i], next);
                    const auto b = two.feed(chart[i], next);
                    CHECK(same_bar(a.bar, expected.bar) && same_bar(b.bar, expected.bar));
                    CHECK(a.is_complete == expected.is_complete && b.is_complete == expected.is_complete);
                    CHECK(a.sub_bar_count == expected.sub_bar_count
                          && b.sub_bar_count == expected.sub_bar_count);
                    CHECK(same_bar(one.current(), reference.current()));
                    CHECK(same_bar(two.current(), reference.current()));
                    CHECK(same_bar(one.last_completed(), reference.last_completed()));
                    CHECK(same_bar(two.last_completed(), reference.last_completed()));
                }
                check_external_census(one, two);
                // Real shared lifetime: dropping the local handles does not
                // destroy the sources until the aggregators release them.
                ordered.reset();
                rescanned.reset();
                check_external_census(one, two);
                CHECK(!one.set_native_period_source(nullptr));
                check_external_census(one, two);
                CHECK(!two.set_native_period_source(nullptr));
                check_external_census(one, two);
                CHECK(external_storage(one, two).resident_bytes() == 0);
            }
        }
    }
}

void multi_day_allocation_failures_and_untracked_owners() {
    namespace md = source::multi_day_detail;
    AllocationCensus census;
    {
        auto params = md::make_owned<md::Params>();
        check_external_census(*params);
        const auto baseline = global_allocation::live_bytes;
        const auto before = global_allocation::allocations;
        { auto value = md::make_owned<md::Source>(params, int64_t{0}, int64_t{kMsPerDay}); }
        const auto construction_requests = global_allocation::allocations - before;
        CHECK(construction_requests > 0);
        CHECK(global_allocation::live_bytes == baseline);
        for (std::size_t fail_at = 0; fail_at < construction_requests; ++fail_at) {
            global_allocation::fail_after = fail_at;
            const bool refused = throws<std::bad_alloc>([&] {
                (void)md::make_owned<md::Source>(params, int64_t{0}, int64_t{kMsPerDay});
            });
            global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
            CHECK(refused);
            CHECK(global_allocation::live_bytes == baseline);
            check_external_census(*params);
        }
        {
            // Passing an unrelated allocator to an untracked construction must
            // not make the unrecorded shared allocation look complete.
            OwnedAllocator<md::Source> unrelated;
            auto untracked = std::make_shared<md::Source>(
                unrelated, params, int64_t{0}, int64_t{kMsPerDay});
            const auto usage = external_storage(*untracked);
            CHECK(!usage.complete());
            CHECK(usage.unsupported & static_cast<std::uint64_t>(OwnedStorageUnsupported::allocator));
            CHECK(usage.storage_bytes() == std::numeric_limits<std::uint64_t>::max());
        }
        CHECK(global_allocation::live_bytes == baseline);
        check_external_census(*params);
    }
}

void unsupported_period_source_is_incomplete_and_unqueried() {
    struct Legacy final : TimeframeAggregator::PeriodSource {
        mutable std::size_t stamp_queries = 0, last_queries = 0, held_queries = 0;
        std::vector<int64_t> opaque;
        explicit Legacy(std::size_t capacity) { opaque.reserve(capacity); }
        bool stamp_at(int64_t ms, int64_t& stamp, bool& last) override {
            ++stamp_queries;
            stamp = 0;
            last = true;
            return ms >= 0;
        }
        int64_t last_stamp() const override { ++last_queries; return 0; }
        std::size_t stamps_held() const override { ++held_queries; return opaque.size(); }
    };
    for (const std::size_t capacity : {31u, 257u, 1031u}) {
        AllocationCensus census;
        {
            auto legacy = std::make_shared<Legacy>(capacity);
            TimeframeAggregator aggregator("D", "1");
            CHECK(aggregator.set_native_period_source(legacy));
            const auto queries = legacy->last_queries;
            const auto usage = external_storage(aggregator, aggregator);
            CHECK(!usage.complete());
            CHECK(usage.unsupported & static_cast<std::uint64_t>(OwnedStorageUnsupported::opaque_type));
            CHECK(usage.storage_bytes() == std::numeric_limits<std::uint64_t>::max());
            CHECK(legacy->stamp_queries == 0 && legacy->held_queries == 0);
            CHECK(legacy->last_queries == queries);
            CHECK(usage.resident_bytes() < global_allocation::live_bytes);
        }
    }
}

template<class T>
void ring_growth_shared_copy_and_release(const T& sample) {
    AllocationCensus census;
    {
        DynamicRingBuffer<T> ring(2051);
        check_external_census(ring);
        CHECK(external_storage(ring).resident_bytes() == 0); // still lazy
        ring.push_front(sample);
        const auto first = external_storage(ring).resident_bytes();
        CHECK(first > 0);
        check_external_census(ring);
        {
            auto copy = ring;
            check_external_census(ring, copy);
            const auto shared = external_storage(ring, copy).resident_bytes();
            // Independently observed global allocations distinguish sharing
            // from summing the two complete per-owner observations.
            CHECK(shared < external_storage(ring).resident_bytes()
                           + external_storage(copy).resident_bytes());
            copy.update_front(sample); // detach the shared current page
            CHECK(external_storage(ring, copy).resident_bytes() > shared);
            check_external_census(ring, copy);
            copy.resize(73);
            check_external_census(ring, copy);
        }
        CHECK(external_storage(ring).resident_bytes() == first);
        for (int i = 0; i < 2051; ++i) ring.push_front(sample);
        CHECK(external_storage(ring).resident_bytes() > first);
        check_external_census(ring);
        ring.clear(); // the directory retains its actual allocated capacity
        check_external_census(ring);
        CHECK(external_storage(ring).resident_payload_bytes == 0);
        CHECK(external_storage(ring).resident_metadata_bytes > 0);
        ring.resize(0);
        check_external_census(ring);
        CHECK(external_storage(ring).resident_bytes() == 0);
    }
}
void series_and_container_owned_allocation_census() {
    ring_growth_shared_copy_and_release(3.25);
    ring_growth_shared_copy_and_release(true);
    const std::string caller_value(257, 's'); // caller-owned, outside census
    ring_growth_shared_copy_and_release(caller_value);
    AllocationCensus census;
    {
        OwnedDeque<std::string> deque;
        OwnedUnorderedMap<std::string, std::tuple<std::vector<std::string>, std::vector<bool>>> helpers;
        check_external_census(deque, helpers);
        for (int i = 0; i < 193; ++i) {
            deque.emplace_back(73 + i, 'd');
            auto& value = helpers[std::string(61, 'k') + std::to_string(i)];
            auto& strings = std::get<0>(value);
            strings.reserve(7 + i % 11); // unused vector capacity must be charged
            strings.emplace_back(89 + i, 'v');
            std::get<1>(value).resize(117 + i % 37, true);
        }
        const auto full = external_storage(deque, helpers).resident_bytes();
        CHECK(full > 193 * 73);
        check_external_census(deque, helpers);
        auto copy = helpers; // separate node/bucket ownership and nested values
        check_external_census(deque, helpers, copy);
        {
            OwnedUnorderedMap<std::string,
                std::tuple<std::vector<std::string>, std::vector<bool>>> assigned;
            assigned = helpers;
            auto moved = std::move(assigned);
            CHECK(helpers.get_allocator() != moved.get_allocator());
            CHECK(assigned.get_allocator() != moved.get_allocator());
            check_external_census(deque, helpers, copy, assigned, moved);
            assigned = std::move(moved);
            CHECK(assigned.get_allocator() != moved.get_allocator());
            check_external_census(deque, helpers, copy, assigned, moved);
            auto deque_copy = deque;
            auto deque_moved = std::move(deque_copy);
            CHECK(deque_copy.get_allocator() != deque_moved.get_allocator());
            check_external_census(deque, helpers, copy, assigned, moved, deque_copy, deque_moved);
            deque_copy = deque_moved;
            CHECK(deque_copy.get_allocator() != deque_moved.get_allocator());
            check_external_census(deque, helpers, copy, assigned, moved, deque_copy, deque_moved);
        }
        helpers.clear();
        deque.clear();
        check_external_census(deque, helpers, copy);
        copy.clear();
        check_external_census(deque, helpers, copy);
        CHECK(external_storage(deque, helpers, copy).resident_bytes() < full);
        // clear() is not a promise to release buckets or deque blocks.
        CHECK(external_storage(deque, helpers, copy).resident_bytes() > 0);
    }
}
void nested_capacity_and_first_member_identity_census() {
    struct Box {
        std::vector<std::string> values; // at the containing object's address
        void observe_owned_storage(OwnedStorage& out) const noexcept {
            if (out.visit(*this)) pineforge::observe_owned_storage(out, values);
        }
    };
    AllocationCensus census;
    {
        std::vector<Box> boxes(3); // first Box is also at the parent allocation
        std::array<std::optional<std::vector<std::vector<int>>>, 2> nested;
        nested[0].emplace(3);
        for (int i = 0; i < 3; ++i) {
            boxes[i].values.reserve(19 + i);
            boxes[i].values.emplace_back(151 + i, 'n');
            (*nested[0])[i].reserve(71 + i);
            (*nested[0])[i].push_back(i);
        }
        check_external_census(boxes, nested);
        const auto full = external_storage(boxes, nested).resident_bytes();
        boxes[0].values.clear(); // retain the reserved vector allocation
        check_external_census(boxes, nested);
        nested[0].reset();
        check_external_census(boxes, nested);
        CHECK(external_storage(boxes, nested).resident_bytes() < full);
    }
}
template<class Make, class Step>
void ta_allocation_census(Make make, Step step) {
    AllocationCensus census;
    {
        auto value = make();
        check_external_census(value);
        for (int i = 0; i < 1031; ++i) step(value, i);
        check_external_census(value);
        {
            auto checkpoint = value;
            check_external_census(value, checkpoint);
            step(value, 1031);
            step(checkpoint, 1031);
            check_external_census(value, checkpoint);
        }
        check_external_census(value);
    }
}
void ta_window_length_and_saved_state_census() {
    ta_allocation_census([] { return ta::SMA(1027); },
        [](auto& v, int i) { v.compute(1.0 + i); });
    ta_allocation_census([] { return ta::BB(131, 2.0); },
        [](auto& v, int i) { v.compute(1.0 + i); });
    ta_allocation_census([] { return ta::HMA(131); },
        [](auto& v, int i) { v.compute(1.0 + i); });
    ta_allocation_census([] { return ta::MFI(131); },
        [](auto& v, int i) { v.compute(1.0 + i, 5.0 + i % 7); });
    ta_allocation_census([] { return ta::CMO(131); },
        [](auto& v, int i) { v.compute(1.0 + i % 17); });
    ta_allocation_census([] { return ta::RCI(37); },
        [](auto& v, int i) { v.compute(1.0 + i % 17); });
    ta_allocation_census([] { return ta::Median(37); },
        [](auto& v, int i) { v.compute(1.0 + i % 17); });
    ta_allocation_census([] { return ta::Mode(37); },
        [](auto& v, int i) { v.compute(1.0 + i % 17); });
    ta_allocation_census([] { return ta::PercentileLinearInterpolation(37); },
        [](auto& v, int i) { v.compute(1.0 + i % 17, 63.0); });
    ta_allocation_census([] { return source::FirstCallBound<ta::SMA>(); },
        [](auto& v, int i) { v.compute([] { return ta::SMA(131); }, 1.0 + i); });
    ta_allocation_census([] { return source::SeriesHighest(); },
        [](auto& v, int i) { v.compute(1.0 + i % 17, 31.0 + i % 7); });
    ta_allocation_census([] { return source::PineSupertrend(); },
        [](auto& v, int i) { v.compute(3.0, 7.0, 10.0 + i, 8.0 + i, 9.0 + i); });
}

struct OwnedState : RuntimeContextEvaluator {
    Series<double> history{2051};
    ta::SMA average{131};
    OwnedUnorderedMap<std::string, std::vector<std::string>> helpers;
    std::tuple<std::vector<double>, std::vector<bool>, std::string> result;
    void consume(RuntimeContextService&, RuntimeContextHandle,
                 const RuntimeContextEvent& event) override {
        history.push(event.bar.close);
        std::get<0>(result).push_back(average.compute(event.bar.close));
        std::get<1>(result).push_back(event.index % 2 == 0);
        std::get<2>(result).assign(173 + event.index % 37, 'r');
        helpers["helper " + std::to_string(event.index % 7)].emplace_back(
            97 + event.index % 53, 'h');
    }
    void shrink() {
        history.clear();
        helpers.clear();
        helpers.rehash(0);
        std::vector<double>().swap(std::get<0>(result));
        std::vector<bool>().swap(std::get<1>(result));
        std::string().swap(std::get<2>(result));
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept override {
        out.metadata(sizeof(*this));
        observe_owned_members(out, history, average, helpers, result);
    }
    std::uint64_t storage_bytes() const noexcept override {
        OwnedStorage out;
        observe_owned_storage(out);
        return out.usage().storage_bytes();
    }
};
void check_service_census(const RuntimeContextService& service) {
    const auto usage = service.usage();
    CHECK(usage.owned_storage.complete());
    CHECK(usage.resident_bytes == global_allocation::live_bytes);
    CHECK(usage.accounted_bytes == usage.resident_bytes + usage.owned_storage.backing_bytes);
}
void live_nested_shared_growth_and_shrink_census() {
    AllocationCensus census;
    {
        RuntimeContextService service;
        service.configure({0, std::nullopt}); // direct-spill, no hard limit
        service.begin_live();
        OwnedState* parent = nullptr;
        const auto h = service.get_or_create({101, "parent", {}}, [&] {
            auto value = std::make_unique<OwnedState>();
            parent = value.get();
            return value;
        });
        for (int i = 0; i < 13; ++i) service.append_live(bar(i));
        service.advance(h, 13);
        check_service_census(service);
        const auto first = service.usage().resident_bytes;
        OwnedState* child = nullptr;
        const auto nested = service.get_or_create({101, "parent", h}, [&] {
            auto value = std::make_unique<OwnedState>(*parent);
            child = value.get(); // Series pages share, containers deep-copy
            return value;
        });
        CHECK(nested != h && service.usage().contexts == 2);
        check_service_census(service);
        // Summing standalone evaluator totals would count shared pages twice.
        const auto separate = parent->storage_bytes() + child->storage_bytes();
        {
            OwnedStorage joined;
            parent->observe_owned_storage(joined);
            child->observe_owned_storage(joined);
            CHECK(joined.usage().storage_bytes() < separate);
        }
        service.advance(nested, 13);
        for (int i = 13; i < 73; ++i) service.append_live(bar(i));
        service.advance(h, 73);
        service.advance(nested, 73);
        CHECK(service.usage().resident_bytes > first);
        const auto grown = service.usage();
        check_service_census(service);
        parent->shrink();
        child->shrink();
        CHECK(service.usage().resident_bytes < grown.resident_bytes);
        service.release(nested);
        service.release(h);
        CHECK(service.usage().contexts == 0);
        check_service_census(service);
    }
}

struct Growing : Sum {
    void observe_owned_storage(OwnedStorage& out) const noexcept override {
        out.metadata(sizeof(*this));
        pineforge::observe_owned_storage(out, payload);
    }
    void consume(RuntimeContextService& service, RuntimeContextHandle handle,
                 const RuntimeContextEvent& event) override {
        Sum::consume(service, handle, event);
        if (event.index == 1) payload.reserve(8191);
    }
};
void real_owned_growth_enforces_budget_before_publication() {
    auto prepare = [](RuntimeContextService& service, std::optional<std::uint64_t> budget) {
        service.configure({0, budget});
        service.begin_live();
        service.append_live(bar(0));
        service.append_live(bar(1));
        return service.get_or_create({303, "growth", {}},
            [] { return std::make_unique<Growing>(); });
    };
    std::uint64_t exact = 0;
    {
        RuntimeContextService service;
        const auto h = prepare(service, std::nullopt);
        service.advance(h, 2);
        exact = service.usage().accounted_bytes;
        CHECK(service.usage().owned_storage.complete());
    }
    {
        RuntimeContextService service;
        const auto h = prepare(service, exact);
        service.advance(h, 2);
        CHECK(sum(service, h).total == 3);
        CHECK(service.usage().accounted_bytes == exact);
    }
    RuntimeContextService limited;
    const auto h = prepare(limited, exact - 1);
    limited.advance(h, 1);
    const auto before = limited.usage().accounted_bytes;
    bool exhausted = false;
    try { limited.advance(h, 2); }
    catch (const RuntimeHistoryBudgetExhausted& error) {
        exhausted = true;
        CHECK(error.configured == exact - 1);
        CHECK(error.used == before);
        CHECK(error.needed == limited.usage().accounted_bytes - before);
        CHECK(error.needed > 0);
        check_budget_failure(error, exact - 1, before,
                             limited.usage().accounted_bytes - before);
    }
    CHECK(exhausted);
    CHECK(throws<std::logic_error>([&] { limited.published(h); }));
}
// Independent fd witnesses use only syscalls and caller-owned expected bytes.
// In particular, discovery never calls ScratchFile::descriptor(), which would
// borrow a managed file and invalidate the last-close refund control below.
bool fd_has_contents(int fd, const void* expected, std::size_t size) {
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0
        || static_cast<std::uint64_t>(info.st_size) != size) return false;
    const auto* bytes = static_cast<const unsigned char*>(expected);
    std::array<unsigned char, 4096> transfer{};
    for (std::size_t offset = 0; offset < size;) {
        const auto count = std::min(transfer.size(), size - offset);
        ssize_t got;
        do { got = ::pread(fd, transfer.data(), count, static_cast<off_t>(offset)); }
        while (got < 0 && errno == EINTR);
        if (got != static_cast<ssize_t>(count)
            || std::memcmp(transfer.data(), bytes + offset, count) != 0) return false;
        offset += count;
    }
    return true;
}
int find_scratch_fd(const void* expected, std::size_t size) {
    DIR* directory = ::opendir("/dev/fd");
    CHECK(directory != nullptr);
    if (!directory) return -1;
    int result = -1;
    while (const auto* entry = ::readdir(directory)) {
        char* end = nullptr;
        const long value = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || value < 0
            || value > std::numeric_limits<int>::max() || value == ::dirfd(directory)) continue;
        const auto fd = static_cast<int>(value);
        struct stat info{};
        if (::fstat(fd, &info) == 0 && info.st_nlink == 0
            && fd_has_contents(fd, expected, size)) {
            CHECK(result == -1); // complete contents must identify one test backing
            result = fd;
        }
    }
    CHECK(::closedir(directory) == 0);
    CHECK(result >= 0);
    return result;
}
struct ScratchFdWitness {
    int fd;
    struct stat before{};
    off_t offset;
    int fd_flags, open_flags;
    explicit ScratchFdWitness(int value) : fd(value),
        offset(::lseek(fd, 0, SEEK_CUR)), fd_flags(::fcntl(fd, F_GETFD)),
        open_flags(::fcntl(fd, F_GETFL)) {
        CHECK(::fstat(fd, &before) == 0 && before.st_size >= 0);
        CHECK(offset >= 0 && fd_flags >= 0 && open_flags >= 0);
    }
    std::uint64_t extent() const { return static_cast<std::uint64_t>(before.st_size); }
    void unchanged() const {
        struct stat after{};
        CHECK(::fstat(fd, &after) == 0);
        CHECK(after.st_dev == before.st_dev && after.st_ino == before.st_ino);
        CHECK(after.st_size == before.st_size && after.st_nlink == before.st_nlink);
        CHECK(after.st_mode == before.st_mode && after.st_mtime == before.st_mtime);
        CHECK(after.st_ctime == before.st_ctime);
        CHECK(::lseek(fd, 0, SEEK_CUR) == offset);
        CHECK(::fcntl(fd, F_GETFD) == fd_flags && ::fcntl(fd, F_GETFL) == open_flags);
    }
};
template<class... T>
OwnedStorageUsage check_plain_scratch_census(std::uint64_t fd_extents, const T&... values) {
    const auto retained = global_allocation::live_bytes;
    const auto observed = external_storage(values...);
    CHECK(global_allocation::live_bytes == retained);
    CHECK(observed.complete());
    CHECK(observed.resident_bytes() == retained);
    CHECK(observed.backing_bytes == fd_extents);
    return observed;
}
std::string scratch_directory_input(std::size_t repeats) {
    std::string result = ".";
    for (std::size_t i = 0; i < repeats; ++i) result += "/.";
    result.reserve(result.size() + 7 * repeats);
    return result;
}
void plain_scratch_directory_fd_lifecycle_census() {
    using detail::ScratchFile;
    CHECK(detail::mode() == detail::QuotaMode::unmanaged);
    CHECK(detail::ScratchArena::attached() == nullptr);
    for (const auto count : {41U, 173U, 509U}) {
        const auto directory = scratch_directory_input(count); // caller-owned
        const auto extent = static_cast<std::size_t>(detail::ScratchArena::kChunkBytes)
            + 17 * count + 19;
        std::vector<unsigned char> image(extent), sparse(extent + count, 0);
        for (std::size_t i = 0; i < image.size(); ++i)
            image[i] = static_cast<unsigned char>((i * 13 + count) % 251 + 1);
        // Both the ordinary and explicitly bounded-cache fd policies retain
        // the same C++ ownership contract; neither is a residency measurement.
        for (const bool bounded_cache : {false, true}) {
            AllocationCensus census;
            {
                ScratchFile file;
                check_plain_scratch_census(0, file);
                file.set_directory(directory);
                const auto directory_bytes = global_allocation::live_bytes;
                CHECK(directory_bytes > directory.size());
                check_plain_scratch_census(0, file);
                file.set_directory(std::string{});
                CHECK(global_allocation::live_bytes == directory_bytes);
                check_plain_scratch_census(0, file); // empty string retains capacity
                file.set_directory(directory);
                if (bounded_cache) file.bound_cache();
                file.open("owned-file");
                const int fd = file.descriptor(); // unmanaged test witness only
                CHECK(file.disk_backed());
                CHECK(::lseek(fd, 7, SEEK_SET) == 7);
                {
                    const ScratchFdWitness witness(fd);
                    check_plain_scratch_census(witness.extent(), file, file);
                    witness.unchanged();
                    CHECK(fd_has_contents(fd, nullptr, 0));
                }
                file.write(0, image.data(), image.size());
                CHECK(fd_has_contents(fd, image.data(), image.size()));
                {
                    const ScratchFdWitness witness(fd);
                    const auto once = check_plain_scratch_census(witness.extent(), file);
                    const auto twice = check_plain_scratch_census(witness.extent(), file, file);
                    check_same_storage(once, twice);
                    OwnedStorage direct;
                    errno = EDOM;
                    file.observe_owned_storage(direct);
                    CHECK(errno == EDOM);
                    witness.unchanged();
                    CHECK(fd_has_contents(fd, image.data(), image.size()));
                }
                file.truncate(count);
                CHECK(fd_has_contents(fd, image.data(), count));
                check_plain_scratch_census(ScratchFdWitness(fd).extent(), file);
                // A writer outside ScratchFile changes the real extent. A
                // cached logical/quota extent cannot be this test's oracle.
                CHECK(::ftruncate(fd, static_cast<off_t>(sparse.size())) == 0);
                std::copy_n(image.data(), count, sparse.data());
                CHECK(fd_has_contents(fd, sparse.data(), sparse.size()));
                check_plain_scratch_census(ScratchFdWitness(fd).extent(), file);
                file.write(0, image.data(), image.size());
                file.truncate(image.size());
                file.release_prefix(detail::ScratchArena::kChunkBytes);
                const auto tail = image.size() - detail::ScratchArena::kChunkBytes;
                CHECK(fd_has_contents(fd, image.data() + detail::ScratchArena::kChunkBytes, tail));
                check_plain_scratch_census(ScratchFdWitness(fd).extent(), file);
                ScratchFile moved(std::move(file));
                CHECK(!file.opened() && moved.disk_backed());
                check_plain_scratch_census(ScratchFdWitness(fd).extent(), file, moved);
                ScratchFile assigned;
                assigned.set_directory(directory);
                assigned.open("owned-replaced");
                const int replaced = assigned.descriptor();
                assigned.write(0, image.data(), count);
                check_plain_scratch_census(ScratchFdWitness(fd).extent()
                    + ScratchFdWitness(replaced).extent(), file, moved, assigned);
                assigned = std::move(moved);
                CHECK(::fcntl(replaced, F_GETFD) == -1 && errno == EBADF);
                check_plain_scratch_census(ScratchFdWitness(fd).extent(), file, moved, assigned);
                CHECK(fd_has_contents(fd, image.data() + detail::ScratchArena::kChunkBytes, tail));
                assigned.clear();
                CHECK(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
                CHECK(!assigned.opened() && global_allocation::live_bytes > 0);
                check_plain_scratch_census(0, file, moved, assigned);
                assigned.open("owned-reopened");
                assigned.write(0, image.data(), count + 1);
                const int reopened = assigned.descriptor();
                const ScratchFdWitness reopened_witness(reopened);
                check_plain_scratch_census(reopened_witness.extent(), file, moved, assigned);
                reopened_witness.unchanged();
                CHECK(fd_has_contents(reopened, image.data(), count + 1));
                assigned.clear();
                assigned.set_directory(std::string{});
                check_plain_scratch_census(0, file, moved, assigned);
            }
        }
    }
}
void scratch_extent_and_observer_failures_are_incomplete() {
    const auto directory = scratch_directory_input(97);
    AllocationCensus census;
    {
        detail::ScratchFile file;
        file.set_directory(directory);
        file.open("owned-query-failure");
        const std::array<unsigned char, 5> image{{9, 13, 27, 41, 3}};
        file.write(0, image.data(), image.size());
        const int fd = file.descriptor();
        const int saved = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
        CHECK(saved >= 0);
        CHECK(::close(fd) == 0);
        const auto failed = external_storage(file);
        CHECK(!failed.complete());
        CHECK(failed.unsupported & static_cast<std::uint64_t>(OwnedStorageUnsupported::scratch_extent));
        CHECK(failed.storage_bytes() == std::numeric_limits<std::uint64_t>::max());
        CHECK(failed.resident_bytes() == global_allocation::live_bytes && failed.resident_bytes() > 0);
        CHECK(::dup2(saved, fd) == fd);
        CHECK(::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
        CHECK(::close(saved) == 0);
        check_plain_scratch_census(ScratchFdWitness(fd).extent(), file);
        OwnedStorage refused;
        global_allocation::refusing = true;
        errno = ERANGE;
        file.observe_owned_storage(refused);
        const int observed_errno = errno;
        global_allocation::refusing = false;
        CHECK(observed_errno == ERANGE);
        CHECK(!refused.usage().complete());
        CHECK(refused.usage().unsupported
              & static_cast<std::uint64_t>(OwnedStorageUnsupported::observation_allocation));
        CHECK(fd_has_contents(fd, image.data(), image.size()));
        check_plain_scratch_census(ScratchFdWitness(fd).extent(), file);
    }
}
auto cache_counters(const detail::ReadThroughCacheStats& s) {
    return std::make_tuple(s.get_calls, s.put_calls, s.pin_calls, s.hits, s.misses,
        s.bypass_reads, s.backing_read_bytes, s.backing_write_bytes, s.evictions,
        s.dirty_evictions, s.allocation_bypass_count, s.current_pins, s.pin_high_water,
        s.current_payload_bytes, s.peak_payload_bytes, s.current_metadata_bytes,
        s.peak_metadata_bytes, s.current_bytes, s.peak_bytes, s.io_errors, s.flush_errors);
}
auto window_io_counters(const WindowCacheStats& s) {
    return std::make_tuple(s.backing_reads, s.backing_read_bytes, s.backing_writes,
        s.backing_write_bytes, s.backing_read_attempts, s.backing_write_attempts, s.io_errors);
}
void scratch_backing_and_cache_observations_are_separate() {
    for (const auto pages : {0U, 1U, 2U}) {
        const std::size_t size = 3073 + pages * 2048;
        const std::size_t index = 17 + pages * 1024;
        std::vector<double> expected(size, 0.0), detached(size, 0.0); // outside census
        expected[index] = 7.25 + pages;
        detached = expected;
        detached[1] = 9.5 + pages;
        AllocationCensus census;
        {
            detail::WindowValues<double> values(size, 0.0, {pages});
            values.write(index, expected[index]);
            auto peer = values;
            const int fd = find_scratch_fd(expected.data(), expected.size() * sizeof(double));
            const ScratchFdWitness witness(fd);
            const auto cold = check_plain_scratch_census(witness.extent(), values, peer);
            check_same_storage(cold, external_storage(peer, values, values, peer));
            CHECK(cold.backing_bytes == external_storage(values).backing_bytes);
            CHECK(cold.logical_payload_bytes == 2 * size * sizeof(double));
            CHECK(values.cache_stats().storage_complete && peer.cache_stats().storage_complete);
            witness.unchanged();
            const auto before_read = global_allocation::live_bytes;
            CHECK(values.read(index) == expected[index]);
            const auto cache = values.read_cache_stats();
            CHECK((cache.current_bytes > 0) == (pages != 0));
            CHECK(global_allocation::live_bytes - before_read == cache.current_bytes);
            const auto warm = check_plain_scratch_census(witness.extent(), values, peer);
            CHECK(warm.resident_payload_bytes - cold.resident_payload_bytes == cache.current_payload_bytes);
            CHECK(warm.resident_metadata_bytes - cold.resident_metadata_bytes == cache.current_metadata_bytes);
            CHECK(warm.workspace_bytes == sizeof(double) * detail::WindowValues<double>::kChunk);
            CHECK(peer.read(index) == expected[index]);
            CHECK(global_allocation::live_bytes - before_read
                  == cache.current_bytes + peer.read_cache_stats().current_bytes);
            const auto first_cache = cache_counters(values.read_cache_stats());
            const auto second_cache = cache_counters(peer.read_cache_stats());
            const auto first_io = window_io_counters(values.cache_stats());
            const auto second_io = window_io_counters(peer.cache_stats());
            check_plain_scratch_census(witness.extent(), values, peer);
            CHECK(first_cache == cache_counters(values.read_cache_stats()));
            CHECK(second_cache == cache_counters(peer.read_cache_stats()));
            CHECK(first_io == window_io_counters(values.cache_stats()));
            CHECK(second_io == window_io_counters(peer.cache_stats()));
            witness.unchanged();
            CHECK(fd_has_contents(fd, expected.data(), expected.size() * sizeof(double)));
            peer.write(1, detached[1]);
            const int peer_fd = find_scratch_fd(detached.data(), detached.size() * sizeof(double));
            CHECK(peer_fd != fd);
            const ScratchFdWitness peer_witness(peer_fd);
            check_plain_scratch_census(witness.extent() + peer_witness.extent(), values, peer);
            CHECK(values.read(1) == expected[1] && peer.read(1) == detached[1]);
            CHECK(values.read(index) == expected[index] && peer.read(index) == detached[index]);
            values.reset(0, 0.0);
            CHECK(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
            check_plain_scratch_census(peer_witness.extent(), values, peer);
            peer.reset(0, 0.0);
            CHECK(::fcntl(peer_fd, F_GETFD) == -1 && errno == EBADF);
            check_plain_scratch_census(0, values, peer);
        }
    }
}
struct ScratchObservationArena {
    detail::ScratchFile backing;
    struct sigaction previous_signal{};
    explicit ScratchObservationArena(std::uint64_t bytes) {
        CHECK(detail::ScratchArena::attached() == nullptr);
        backing.set_directory(".");
        backing.open("owned-borrowed-arena");
        backing.truncate(bytes);
        CHECK(::sigaction(SIGXFSZ, nullptr, &previous_signal) == 0);
        detail::ScratchArena::attach(backing.descriptor(), bytes);
    }
    ~ScratchObservationArena() {
        detail::ScratchArena::detach();
        CHECK(::sigaction(SIGXFSZ, &previous_signal, nullptr) == 0);
    }
};
void scratch_arena_keeps_known_directory_and_unknown_borrowed_owner() {
    using detail::ScratchArena;
    ScratchObservationArena attachment(11 * ScratchArena::kChunkBytes + 79);
    const ScratchFdWitness borrowed(attachment.backing.descriptor());
    for (const auto chunks : {1U, 3U, 7U}) {
        const auto directory = scratch_directory_input(37 * chunks);
        AllocationCensus census; // the shared arena is an outside owner
        {
            detail::ScratchFile file;
            file.set_directory(directory);
            const auto directory_bytes = global_allocation::live_bytes;
            file.open_bounded("owned-region", chunks * ScratchArena::kChunkBytes - 13);
            const auto chunk_directory_bytes = global_allocation::live_bytes - directory_bytes;
            CHECK(chunk_directory_bytes > 0);
            const auto check_region = [&] {
                const ScratchFdWitness current_borrowed(borrowed.fd);
                const auto before = ScratchArena::attached()->stats();
                const auto observed = external_storage(file, file);
                const auto after = ScratchArena::attached()->stats();
                CHECK(!observed.complete());
                CHECK(observed.unsupported
                      & static_cast<std::uint64_t>(OwnedStorageUnsupported::scratch_arena));
                CHECK(observed.storage_bytes() == std::numeric_limits<std::uint64_t>::max());
                CHECK(observed.resident_metadata_bytes == chunk_directory_bytes);
                CHECK(observed.resident_payload_bytes == directory_bytes);
                CHECK(observed.resident_bytes() <= global_allocation::live_bytes);
                CHECK(observed.backing_bytes == 0); // attached fd is borrowed, not owned again
                CHECK(before.bytes_used == after.bytes_used && before.chunks_free == after.chunks_free);
                CHECK(before.windows_mapped_bytes == after.windows_mapped_bytes);
                CHECK(before.high_water == after.high_water);
                CHECK(current_borrowed.extent() == borrowed.extent());
                current_borrowed.unchanged();
            };
            check_region(); // reserved directory with no live chunks
            const unsigned char value = static_cast<unsigned char>(chunks + 37);
            const auto offset = chunks * ScratchArena::kChunkBytes - 17;
            file.write(offset, &value, 1);
            check_region();
            unsigned char received = 0;
            file.read(offset, &received, 1);
            CHECK(received == value);
            file.truncate(0);
            check_region(); // empty bounded region retains the actual reserved directory
            file.clear();
            CHECK(!file.opened());
            check_plain_scratch_census(0, file); // its arena binding has actually ended
        }
    }
}
void scratch_slab_and_custom_descendants_stay_incomplete() {
    using detail::ScratchResource;
    for (const auto count : {43U, 181U, 521U}) {
        const auto directory = scratch_directory_input(count);
        AllocationCensus census;
        {
            detail::ScratchManifest manifest;
            manifest.U = count;
            auto slab = std::make_shared<detail::ScratchSlab>(manifest);
            detail::ScratchFile one, two;
            one.set_directory(directory);
            two.set_directory(directory);
            one.bind_scratch(slab, ScratchResource::cohort);
            two.bind_scratch(slab, ScratchResource::cohort);
            const auto shared = slab.use_count();
            const std::array<unsigned char, 5> image{{7, 31, 53, 17, 2}};
            one.write(count, image.data(), image.size());
            const auto before = slab->region(ScratchResource::cohort).logical_extent;
            const auto observed = external_storage(one, two, one);
            CHECK(!observed.complete());
            CHECK(observed.unsupported
                  & static_cast<std::uint64_t>(OwnedStorageUnsupported::scratch_slab));
            CHECK(observed.storage_bytes() == std::numeric_limits<std::uint64_t>::max());
            CHECK(observed.resident_payload_bytes > 0);
            CHECK(observed.resident_bytes() < global_allocation::live_bytes);
            CHECK(observed.backing_bytes == 0);
            CHECK(slab.use_count() == shared);
            CHECK(slab->region(ScratchResource::cohort).logical_extent == before);
            std::array<unsigned char, 5> received{};
            two.read(count, received.data(), received.size());
            CHECK(received == image);
            one.clear(); // deliberately retains the shared slab binding
            CHECK(!external_storage(one).complete());
            CHECK(slab.use_count() == shared);
            one.bind_scratch({}, ScratchResource::cohort);
            two.bind_scratch({}, ScratchResource::cohort);
            slab.reset();
            check_plain_scratch_census(0, one, two);

            struct Custom { std::vector<unsigned char> hidden; };
            Custom custom;
            custom.hidden.reserve(5 * count + 11);
            const auto incomplete = external_storage(one, two, custom);
            CHECK(!incomplete.complete());
            CHECK(incomplete.unsupported
                  & static_cast<std::uint64_t>(OwnedStorageUnsupported::opaque_type));
            CHECK(incomplete.resident_bytes() > 0);
            CHECK(incomplete.resident_bytes() < global_allocation::live_bytes);
            CHECK(incomplete.storage_bytes() == std::numeric_limits<std::uint64_t>::max());
        }
    }
}
// This entry requires a real managed host (including admitted inode COMMITs).
// Invoke it separately with --managed-scratch-observation. An ordinary
// invocation supplies no managed result; no endpoint or host verdict is mocked.
void managed_scratch_observation_traverses_endpoint_and_preserves_last_close() {
    using namespace pineforge::detail;
    CHECK(mode() == QuotaMode::managed);
    CHECK(ScratchArena::attached() == nullptr);
    auto quota = RunQuota::from_environment(); // trusted test client, outside the census
    const auto endpoint = external_storage(quota);
    CHECK(endpoint.complete() && endpoint.resident_metadata_bytes > 0);
    CHECK(endpoint.resident_payload_bytes == 0 && endpoint.backing_bytes == 0);
    for (const auto count : {47U, 191U, 541U}) {
        const auto directory = scratch_directory_input(count);
        std::vector<unsigned char> image(31 * count + 7);
        for (std::size_t i = 0; i < image.size(); ++i)
            image[i] = static_cast<unsigned char>((i * 23 + count) % 251 + 1);
        auto other_image = image;
        other_image.push_back(static_cast<unsigned char>(count % 251));
        const auto baseline_bytes = quota.snapshot(QuotaResource::scratch_bytes).message[QuotaSlot::used];
        const auto baseline_files = quota.snapshot(QuotaResource::scratch_files).message[QuotaSlot::used];
        AllocationCensus census;
        {
            ScratchFile file;
            file.set_directory(directory);
            file.open("owned-managed");
            file.write(0, image.data(), image.size());
            const int fd = find_scratch_fd(image.data(), image.size());
            CHECK(::lseek(fd, 11, SEEK_SET) == 11);
            const ScratchFdWitness witness(fd);
            ScratchFile other;
            other.set_directory(directory);
            other.open("owned-managed-other");
            other.write(0, other_image.data(), other_image.size());
            const int other_fd = find_scratch_fd(other_image.data(), other_image.size());
            CHECK(::lseek(other_fd, 19, SEEK_SET) == 19);
            const ScratchFdWitness other_witness(other_fd);
            const auto prior = quota.snapshot();
            const auto retained = global_allocation::live_bytes;
            const auto observed = external_storage(file, other);
            const auto repeated = external_storage(quota, other, file, other, quota);
            const auto after = quota.snapshot();
            CHECK(observed.complete() && repeated.complete());
            // The endpoint was allocated before this independent recording
            // window. Only the two files' own requests were recorded here.
            CHECK(observed.resident_bytes() == retained + endpoint.resident_bytes());
            CHECK(observed.resident_metadata_bytes >= 2 * sizeof(ScratchFileQuota));
            CHECK(observed.resident_metadata_bytes == repeated.resident_metadata_bytes);
            CHECK(observed.resident_payload_bytes == repeated.resident_payload_bytes);
            CHECK(observed.backing_bytes == witness.extent() + other_witness.extent());
            CHECK(repeated.backing_bytes == observed.backing_bytes);
            CHECK(global_allocation::live_bytes == retained);
            // These two independent host requests bracket only observation.
            // An observer protocol call creates a sequence gap or changes the
            // host's committed/reserved resource fields.
            CHECK(after.message.sequence == prior.message.sequence + 1);
            CHECK(after.message.slots == prior.message.slots);
            witness.unchanged();
            other_witness.unchanged();
            CHECK(fd_has_contents(fd, image.data(), image.size()));
            CHECK(fd_has_contents(other_fd, other_image.data(), other_image.size()));
            ScratchFile moved(std::move(file));
            const auto before_move_observation = quota.snapshot();
            const auto moved_observation = external_storage(file, other, moved, moved, quota);
            const auto after_move_observation = quota.snapshot();
            CHECK(moved_observation.complete());
            CHECK(moved_observation.resident_metadata_bytes == observed.resident_metadata_bytes);
            CHECK(moved_observation.resident_bytes()
                  == global_allocation::live_bytes + endpoint.resident_bytes());
            CHECK(moved_observation.backing_bytes == observed.backing_bytes);
            CHECK(after_move_observation.message.sequence == before_move_observation.message.sequence + 1);
            CHECK(after_move_observation.message.slots == before_move_observation.message.slots);
            witness.unchanged();
            other_witness.unchanged();
            moved.clear();
            CHECK(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
            other.clear();
            CHECK(::fcntl(other_fd, F_GETFD) == -1 && errno == EBADF);
            // If observation borrowed descriptor(), closed() cannot release
            // this file. The real host's used counts then fail these controls.
            CHECK(quota.snapshot(QuotaResource::scratch_bytes).message[QuotaSlot::used] == baseline_bytes);
            CHECK(quota.snapshot(QuotaResource::scratch_files).message[QuotaSlot::used] == baseline_files);
            check_plain_scratch_census(0, file, moved, other);
        }
    }
    // A retained real refusal must also remain visible through the file bridge.
    // This final scene has no allocation recording window: the active attempt
    // deliberately retains its refusal beyond these local owners' lifetimes.
    const auto baseline_bytes = quota.snapshot(QuotaResource::scratch_bytes).message[QuotaSlot::used];
    const auto baseline_files = quota.snapshot(QuotaResource::scratch_files).message[QuotaSlot::used];
    ScratchFile refused_file;
    refused_file.set_directory(scratch_directory_input(67));
    refused_file.open("owned-managed-refusal");
    std::array<unsigned char, 1031> image{};
    for (std::size_t i = 0; i < image.size(); ++i)
        image[i] = static_cast<unsigned char>((i * 41 + 17) % 251);
    refused_file.write(0, image.data(), image.size());
    const int fd = find_scratch_fd(image.data(), image.size());
    CHECK(::lseek(fd, 23, SEEK_SET) == 23);
    const ScratchFdWitness witness(fd);
    const auto output = quota.snapshot(QuotaResource::output_bytes);
    CHECK(output.message[QuotaSlot::limit] < kRunQuotaMax);
    if (output.message[QuotaSlot::limit] >= kRunQuotaMax) return;
    const auto requested = output.message[QuotaSlot::limit] - output.message[QuotaSlot::used] + 1;
    RunFailureValue caught;
    try {
        (void)quota.reserve({},
            {0, 0, requested, 0}, 0, 0, QuotaStage::execution, QuotaArtifact::json);
    } catch (const coded<std::runtime_error>& error) {
        caught = error.run_failure();
    }
    const auto refusal = quota.refusal();
    CHECK(refusal && caught.code == RunFailureCode::run_resource_quota_exceeded);
    CHECK(refusal && caught.args == refusal->failure.args);
    if (!refusal || !refusal->failure.args) return;
    CHECK(refusal->message[QuotaSlot::requested] == requested);
    CHECK(refusal->message[QuotaSlot::limit] == output.message[QuotaSlot::limit]);
    const auto prior = quota.snapshot(QuotaResource::output_bytes);
    const auto observed = external_storage(refused_file);
    const auto repeated = external_storage(quota, refused_file, refused_file);
    const auto after = quota.snapshot(QuotaResource::output_bytes);
    // The factory-minted refusal JSON is a verified owned domain: the file
    // graph is complete, with the real object and buffer requests included.
    CHECK(observed.complete() && !observed.overflow && repeated.complete());
    CHECK(observed.storage_bytes() == observed.resident_bytes() + observed.backing_bytes);
    CHECK(observed.resident_metadata_bytes >= endpoint.resident_metadata_bytes
          + sizeof(ScratchFileQuota) + sizeof(QuotaSnapshot));
    CHECK(observed.resident_payload_bytes
          >= sizeof(std::string) + refusal->failure.args->capacity() + 1);
    CHECK(observed.resident_metadata_bytes == repeated.resident_metadata_bytes);
    CHECK(observed.resident_payload_bytes == repeated.resident_payload_bytes);
    CHECK(observed.unsupported == repeated.unsupported);
    CHECK(observed.backing_bytes == witness.extent() && repeated.backing_bytes == witness.extent());
    CHECK(after.message.sequence == prior.message.sequence + 1);
    CHECK(after.message.slots == prior.message.slots);
    CHECK(quota.refusal() == refusal && caught.args == refusal->failure.args);
    witness.unchanged();
    CHECK(fd_has_contents(fd, image.data(), image.size()));
    refused_file.clear();
    CHECK(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    CHECK(quota.snapshot(QuotaResource::scratch_bytes).message[QuotaSlot::used] == baseline_bytes);
    CHECK(quota.snapshot(QuotaResource::scratch_files).message[QuotaSlot::used] == baseline_files);
}
void unmanaged_quota_hooks_own_no_endpoint() {
    using namespace pineforge::detail;
    CHECK(mode() == QuotaMode::unmanaged);
    AllocationCensus census;
    auto quota = RunQuota::from_environment();
    auto explicit_client = RunQuota::from_fd(-1);
    QuotaTicket empty;
    const QuotaObject object{UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    const QuotaAmounts amounts{UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    auto ticket = quota.reserve(object, amounts, UINT64_MAX, UINT64_MAX,
                                QuotaStage::execution, QuotaArtifact::json);
    auto moved = std::move(ticket);
    moved.commit(amounts, UINT64_MAX, UINT64_MAX, UINT64_MAX);
    moved.cancel();
    quota.release(object, amounts, UINT64_MAX, UINT64_MAX,
                  QuotaStage::execution, QuotaArtifact::json);
    const auto before = global_allocation::recorded_allocations;
    const auto observed = external_storage(quota, explicit_client, empty, ticket, moved);
    CHECK(observed.complete() && observed.resident_bytes() == 0);
    CHECK(observed.backing_bytes == 0 && !observed.overflow);
    CHECK(global_allocation::recorded_allocations == before);
    CHECK(global_allocation::live_bytes == 0);
}

struct QuotaSocketWitness {
    int inherited, duplicate;
    struct stat identity{};
    int inherited_flags, inherited_status, duplicate_flags, duplicate_status;
    explicit QuotaSocketWitness(int fd) : inherited(fd), duplicate(-1) {
        CHECK(::fstat(fd, &identity) == 0 && S_ISSOCK(identity.st_mode));
        DIR* directory = ::opendir("/dev/fd");
        CHECK(directory != nullptr);
        if (directory) {
            while (const auto* entry = ::readdir(directory)) {
                char* end = nullptr;
                const long number = std::strtol(entry->d_name, &end, 10);
                if (end == entry->d_name || *end || number < 0
                    || number > std::numeric_limits<int>::max()
                    || number == fd || number == ::dirfd(directory)) continue;
                struct stat candidate{};
                const int other = static_cast<int>(number);
                if (::fstat(other, &candidate) == 0 && candidate.st_dev == identity.st_dev
                    && candidate.st_ino == identity.st_ino) {
                    CHECK(duplicate == -1);
                    duplicate = other;
                }
            }
            CHECK(::closedir(directory) == 0);
        }
        CHECK(duplicate >= 0);
        inherited_flags = ::fcntl(fd, F_GETFD);
        inherited_status = ::fcntl(fd, F_GETFL);
        duplicate_flags = ::fcntl(duplicate, F_GETFD);
        duplicate_status = ::fcntl(duplicate, F_GETFL);
        CHECK(inherited_flags >= 0 && inherited_status >= 0 && duplicate_status >= 0);
        CHECK(duplicate_flags >= 0 && (duplicate_flags & FD_CLOEXEC));
    }
    void unchanged() const {
        for (const int fd : {inherited, duplicate}) {
            struct stat now{};
            CHECK(::fstat(fd, &now) == 0);
            CHECK(now.st_dev == identity.st_dev && now.st_ino == identity.st_ino);
        }
        CHECK(::fcntl(inherited, F_GETFD) == inherited_flags);
        CHECK(::fcntl(inherited, F_GETFL) == inherited_status);
        CHECK(::fcntl(duplicate, F_GETFD) == duplicate_flags);
        CHECK(::fcntl(duplicate, F_GETFL) == duplicate_status);
    }
};

void check_quota_observation_protocol(const detail::QuotaSnapshot& before,
                                      const detail::QuotaSnapshot& after) {
    CHECK(after.message.sequence == before.message.sequence + 1);
    CHECK(after.message.opcode == before.message.opcode);
    CHECK(after.message.slots == before.message.slots); // includes both nonces
    CHECK(after.failure.code == before.failure.code);
}

// Fail construction immediately after the real registry-node request. Its
// deletion is observed independently when normal attachment prunes the empty
// weak entry. No map/control-block geometry is inferred from a library type.
std::size_t measured_quota_registry_request(int fd) {
    CHECK(global_allocation::live_bytes == 0);
    const auto allocations = global_allocation::recorded_allocations;
    const auto releases = global_allocation::recorded_releases;
    global_allocation::fail_after = 1;
    const bool failed = throws<std::bad_alloc>([&] { (void)detail::RunQuota::from_fd(fd); });
    global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
    CHECK(failed && global_allocation::recorded_allocations == allocations + 1);
    const auto bytes = global_allocation::live_bytes;
    CHECK(bytes > 0);
    (void)detail::RunQuota::from_environment(); // normal weak-registry pruning
    CHECK(global_allocation::recorded_releases == releases + 1);
    CHECK(global_allocation::live_bytes == 0);
    return bytes;
}

struct QuotaContext : RuntimeContextEvaluator {
    detail::RunQuota quota;
    explicit QuotaContext(detail::RunQuota value) : quota(std::move(value)) {}
    void consume(RuntimeContextService&, RuntimeContextHandle,
                 const RuntimeContextEvent&) override {}
    std::uint64_t storage_bytes() const noexcept override {
        return external_storage(*this).storage_bytes();
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept override {
        if (!out.visit(*this)) return;
        out.metadata(sizeof(*this));
        quota.observe_owned_storage(out);
    }
};

// A quota graph that also carries failure JSON under a control this module
// did not mint: that JSON is unknown, so the whole owner stays incomplete.
struct ForeignJsonQuotaContext : QuotaContext {
    RunFailureValue foreign;
    ForeignJsonQuotaContext(detail::RunQuota value, RunFailureValue json)
        : QuotaContext(std::move(value)), foreign(std::move(json)) {}
    void observe_owned_storage(OwnedStorage& out) const noexcept override {
        if (!out.visit(*this)) return;
        out.metadata(sizeof(*this));
        quota.observe_owned_storage(out);
        foreign.observe_owned_storage(out);
    }
};

// The factory-minted refusal is verified owned storage: the refused owner is
// complete and obeys an ordinary hard budget. The same refusal text under an
// unknown control stays incomplete and refused at every budget.
void refused_quota_budget_controls(const detail::RunQuota& quota) {
    const Bar input = bar(0);
    RuntimeBatchLease lease(&input, 1);
    const RuntimeContextKey key{507, "quota ownership", {}};
    const auto factory = [&] { return std::make_unique<QuotaContext>(quota); };
    RuntimeContextService unlimited;
    unlimited.bind_batch(lease);
    unlimited.accept_batch(1);
    const auto h = unlimited.get_or_create(key, factory);
    unlimited.advance(h, 1);
    (void)unlimited.published(h);
    const auto known = unlimited.usage().owned_storage;
    CHECK(known.complete() && !known.overflow);
    CHECK(known.resident_payload_bytes >= sizeof(std::string));
    {
        RuntimeContextService limited;
        limited.configure({0, 0});
        limited.bind_batch(lease);
        limited.accept_batch(1);
        CHECK(throws<RuntimeHistoryBudgetExhausted>([&] { limited.get_or_create(key, factory); }));
        CHECK(limited.usage().contexts == 0);
    }
    {
        RuntimeContextService limited;
        limited.configure({0, UINT64_MAX});
        limited.bind_batch(lease);
        limited.accept_batch(1);
        const auto accepted = limited.get_or_create(key, factory);
        limited.advance(accepted, 1);
        CHECK(limited.usage().contexts == 1);
        CHECK(limited.usage().owned_storage.complete());
    }
    const auto refusal = quota.refusal();
    CHECK(refusal && refusal->failure.args);
    if (!refusal || !refusal->failure.args) return;
    const RuntimeContextKey foreign_key{508, "foreign quota json", {}};
    const auto foreign_factory = [&] {
        return std::make_unique<ForeignJsonQuotaContext>(quota, RunFailureValue{
            refusal->failure.code, std::make_shared<const std::string>(*refusal->failure.args)});
    };
    RuntimeContextService foreign_unlimited;
    foreign_unlimited.bind_batch(lease);
    foreign_unlimited.accept_batch(1);
    const auto f = foreign_unlimited.get_or_create(foreign_key, foreign_factory);
    foreign_unlimited.advance(f, 1); // NONE still publishes the foreign owner.
    (void)foreign_unlimited.published(f);
    const auto expected = foreign_unlimited.usage().owned_storage;
    CHECK(!expected.complete() && !expected.overflow);
    CHECK(expected.unsupported & static_cast<std::uint64_t>(OwnedStorageUnsupported::allocator));
    CHECK(expected.unsupported & static_cast<std::uint64_t>(OwnedStorageUnsupported::opaque_type));
    for (const auto bound : {std::uint64_t{0}, std::uint64_t{4093}, UINT64_MAX}) {
        RuntimeContextService limited;
        limited.configure({0, bound});
        limited.bind_batch(lease);
        limited.accept_batch(1);
        bool refused = false;
        try { limited.get_or_create(foreign_key, foreign_factory); }
        catch (const RuntimeStorageObservationIncomplete& error) {
            refused = true;
            check_observation_failure(error, expected);
        }
        CHECK(refused && limited.usage().contexts == 0);
    }
}

void check_quota_output_refusal(const detail::QuotaSnapshot& snapshot,
                               const detail::QuotaMessage& before,
                               std::uint64_t requested) {
    using detail::QuotaSlot;
    CHECK(snapshot.failure.code == RunFailureCode::run_resource_quota_exceeded);
    CHECK(snapshot.message[QuotaSlot::used] == before[QuotaSlot::used]);
    CHECK(snapshot.message[QuotaSlot::limit] == before[QuotaSlot::limit]);
    CHECK(snapshot.message[QuotaSlot::peak] == before[QuotaSlot::peak]);
    CHECK(snapshot.message[QuotaSlot::requested] == requested);
    const auto expected = "{\"artifact\":\"json\",\"limit\":"
        + std::to_string(before[QuotaSlot::limit]) + ",\"peak\":"
        + std::to_string(before[QuotaSlot::peak]) + ",\"requested\":"
        + std::to_string(requested)
        + ",\"resource\":\"output_bytes\",\"stage\":\"execution\",\"used\":"
        + std::to_string(before[QuotaSlot::used]) + "}";
    CHECK(snapshot.failure.args && *snapshot.failure.args == expected);
}

// Requires three additional, distinct real host channels, passed as fd argv
// values. The environment's active channel is a fourth, clean pruning anchor.
// Extra channels need output reservation capacity 1/17/257 respectively and
// three output limits of different decimal widths, all < kRunQuotaMax.
// Non-file output tickets use the protocol's null object. No server is mocked.
void managed_quota_endpoint_graph(const std::array<int, 3>& fds) {
    using namespace pineforge::detail;
    CHECK(mode() == QuotaMode::managed);
    auto anchor = RunQuota::from_environment(); // deliberately outside census
    std::array<struct stat, 3> identities{};
    for (std::size_t i = 0; i < fds.size(); ++i) {
        CHECK(::fstat(fds[i], &identities[i]) == 0 && S_ISSOCK(identities[i].st_mode));
        for (std::size_t j = 0; j < i; ++j)
            CHECK(identities[i].st_dev != identities[j].st_dev
                  || identities[i].st_ino != identities[j].st_ino);
    }
    {
        AllocationCensus census;
        std::array<std::size_t, 3> registry{};
        for (std::size_t i = 0; i < fds.size(); ++i)
            registry[i] = measured_quota_registry_request(fds[i]);
        std::array<std::optional<RunQuota>, 3> clients;
        std::array<QuotaTicket, 3> tickets;
        std::array<std::optional<QuotaSocketWitness>, 3> sockets;
        std::array<OwnedStorageUsage, 3> live{};
        std::array<std::size_t, 3> endpoint_requests{};
        const auto registry_bytes = registry[0] + registry[1] + registry[2];
        for (std::size_t i = 0; i < fds.size(); ++i) {
            const auto before = global_allocation::live_bytes;
            clients[i].emplace(RunQuota::from_fd(fds[i]));
            const auto retained = global_allocation::live_bytes - before;
            endpoint_requests[i] = retained - registry[i];
            live[i] = external_storage(*clients[i]);
            CHECK(live[i].complete() && !live[i].overflow);
            CHECK(live[i].resident_bytes() == retained - registry[i]);
            CHECK(live[i].resident_payload_bytes == 0 && live[i].backing_bytes == 0);
            sockets[i].emplace(fds[i]);
            auto copy = *clients[i];
            const auto allocation_events = global_allocation::recorded_allocations;
            auto attached = RunQuota::from_fd(fds[i]);
            CHECK(global_allocation::recorded_allocations == allocation_events);
            check_same_storage(external_storage(copy, attached, *clients[i]), live[i]);
            const auto baseline = copy.snapshot(QuotaResource::output_bytes);
            const std::uint64_t bytes = std::array<std::uint64_t, 3>{1, 17, 257}[i];
            CHECK(baseline.message[QuotaSlot::limit] - baseline.message[QuotaSlot::used] >= bytes);
            tickets[i] = copy.reserve({}, {0, 0, bytes, 0}, 0, 0,
                                      QuotaStage::execution, QuotaArtifact::json);
            const auto ticket_id = tickets[i].id();
            CHECK(ticket_id != 0);
            auto moved = std::move(tickets[i]);
            CHECK(moved.id() == ticket_id);
            CHECK(external_storage(tickets[i]).complete());
            CHECK(external_storage(tickets[i]).resident_bytes() == 0);
            check_same_storage(external_storage(moved, attached, copy), live[i]);
            tickets[i] = std::move(moved);
            CHECK(external_storage(moved).resident_bytes() == 0);
            const auto prior = copy.snapshot(QuotaResource::output_bytes);
            CHECK(prior.message[QuotaSlot::reserved] == baseline.message[QuotaSlot::reserved] + bytes);
            CHECK(prior.message[QuotaSlot::committed] == baseline.message[QuotaSlot::committed]);
            const auto observed = external_storage(tickets[i], copy, attached, tickets[i]);
            check_same_storage(observed, live[i]);
            {
                OwnedStorage refused;
                const auto allocation_refusals = global_allocation::refused;
                global_allocation::refusing = true;
                errno = EDOM;
                copy.observe_owned_storage(refused);
                global_allocation::refusing = false;
                CHECK(errno == EDOM && global_allocation::refused > allocation_refusals);
                CHECK(!refused.usage().complete());
                CHECK(refused.usage().unsupported & static_cast<std::uint64_t>(
                    OwnedStorageUnsupported::observation_allocation));
            }
            const auto after = copy.snapshot(QuotaResource::output_bytes);
            check_quota_observation_protocol(prior, after);
            sockets[i]->unchanged();
            tickets[i].cancel();
            CHECK(tickets[i].id() == 0);
            check_same_storage(external_storage(tickets[i]), live[i]); // still a strong owner
            const auto canceled = copy.snapshot(QuotaResource::output_bytes);
            CHECK(canceled.message[QuotaSlot::used] == baseline.message[QuotaSlot::used]);
            CHECK(canceled.message[QuotaSlot::reserved] == baseline.message[QuotaSlot::reserved]);
            auto moved_client = std::move(*clients[i]);
            CHECK(external_storage(*clients[i]).resident_bytes() == 0);
            *clients[i] = std::move(moved_client);
            CHECK(external_storage(moved_client).resident_bytes() == 0);
            check_same_storage(external_storage(*clients[i]), live[i]);
        }
        const auto joined = external_storage(*clients[0], tickets[2], *clients[1],
                                             tickets[0], *clients[2], tickets[1]);
        CHECK(joined.complete());
        CHECK(joined.resident_bytes() == global_allocation::live_bytes - registry_bytes);
        CHECK(joined.resident_bytes()
              == live[0].resident_bytes() + live[1].resident_bytes() + live[2].resident_bytes());
        std::array<std::shared_ptr<const QuotaSnapshot>, 3> refusals;
        std::array<std::weak_ptr<const QuotaSnapshot>, 3> weak_refusals;
        std::array<QuotaMessage, 3> output_before{};
        std::array<std::uint64_t, 3> requests{};
        for (std::size_t i = 0; i < clients.size(); ++i) {
            auto& quota = *clients[i];
            const auto output = quota.snapshot(QuotaResource::output_bytes);
            output_before[i] = output.message;
            CHECK(output.message[QuotaSlot::limit] < kRunQuotaMax);
            if (output.message[QuotaSlot::limit] >= kRunQuotaMax) return;
            requests[i] = output.message[QuotaSlot::limit] - output.message[QuotaSlot::used] + 1;
            const auto before_refusal = global_allocation::live_bytes;
            bool caught = false;
            try {
                (void)quota.reserve({},
                    {0, 0, requests[i], 0}, 0, 0, QuotaStage::execution, QuotaArtifact::json);
            } catch (const coded<std::runtime_error>& error) {
                caught = true;
                const auto snapshot = quota.refusal();
                CHECK(snapshot && error.run_failure_code() == snapshot->failure.code);
                CHECK(snapshot && error.run_failure().args == snapshot->failure.args);
            }
            CHECK(caught);
            refusals[i] = quota.refusal();
            CHECK(refusals[i] && refusals[i]->failure.args);
            if (!refusals[i] || !refusals[i]->failure.args) return;
            weak_refusals[i] = refusals[i];
            check_quota_output_refusal(*refusals[i], output_before[i], requests[i]);
            for (std::size_t j = 0; j < i; ++j) {
                CHECK(requests[i] != requests[j]);
                CHECK(refusals[i]->failure.args->size() != refusals[j]->failure.args->size());
            }
            const auto strong = refusals[i].use_count();
            const auto json_strong = refusals[i]->failure.args.use_count();
            const auto retained = global_allocation::live_bytes;
            const auto refusal_requests = retained - before_refusal;
            const auto prior = quota.snapshot(QuotaResource::output_bytes);
            errno = ERANGE;
            const auto observed = external_storage(quota, tickets[i], quota);
            CHECK(errno == ERANGE);
            // Independent census: the endpoint plus every request the refusal
            // retained (snapshot and record, JSON ledger, records, object,
            // control and buffer), each counted exactly once.
            CHECK(observed.complete() && !observed.overflow);
            CHECK(observed.storage_bytes() == observed.resident_bytes());
            CHECK(observed.resident_bytes() == endpoint_requests[i] + refusal_requests);
            CHECK(observed.resident_metadata_bytes >= live[i].resident_metadata_bytes + sizeof(QuotaSnapshot));
            CHECK(observed.resident_payload_bytes
                  >= sizeof(std::string) + refusals[i]->failure.args->capacity() + 1);
            const auto after = quota.snapshot(QuotaResource::output_bytes);
            check_quota_observation_protocol(prior, after);
            CHECK(refusals[i].use_count() == strong);
            CHECK(refusals[i]->failure.args.use_count() == json_strong);
            CHECK(quota.refusal() == refusals[i]);
            CHECK(global_allocation::live_bytes == retained);
            sockets[i]->unchanged();
            refused_quota_budget_controls(quota);
            CHECK(throws<coded<std::runtime_error>>([&] { quota.check(); }));
            CHECK(throws<coded<std::runtime_error>>([&] { (void)quota.finish(); }));
            CHECK(quota.refusal() == refusals[i]);
            check_quota_output_refusal(*refusals[i], output_before[i], requests[i]);
        }
        const auto retained = global_allocation::live_bytes;
        for (std::size_t i = 0; i < clients.size(); ++i) {
            clients[i].reset();
            const auto only_ticket = external_storage(tickets[i]);
            const auto repeated_ticket = external_storage(tickets[i], tickets[i]);
            CHECK(!only_ticket.complete() && !repeated_ticket.complete());
            CHECK(only_ticket.unsupported == repeated_ticket.unsupported);
            CHECK(only_ticket.resident_metadata_bytes == repeated_ticket.resident_metadata_bytes);
            CHECK(only_ticket.resident_payload_bytes == repeated_ticket.resident_payload_bytes);
            CHECK(::fcntl(sockets[i]->duplicate, F_GETFD) == sockets[i]->duplicate_flags);
            tickets[i] = QuotaTicket{};
            CHECK(::fcntl(sockets[i]->duplicate, F_GETFD) == -1 && errno == EBADF);
            CHECK(::fcntl(fds[i], F_GETFD) == sockets[i]->inherited_flags);
        }
        CHECK(global_allocation::live_bytes == retained); // weak endpoint blocks still retained
        (void)RunQuota::from_environment(); // prune; no new HELLO on the active anchor
        CHECK(global_allocation::live_bytes > 0 && global_allocation::live_bytes < retained);
        for (std::size_t i = 0; i < refusals.size(); ++i) {
            check_quota_output_refusal(*refusals[i], output_before[i], requests[i]);
            refusals[i].reset();
            CHECK(weak_refusals[i].expired());
        }
        CHECK(global_allocation::live_bytes > 0); // actual snapshot weak/control allocations
        for (auto& weak : weak_refusals) weak.reset();
        CHECK(global_allocation::live_bytes == 0); // ledger has no owning cycle
    }
    const auto prior = anchor.snapshot();
    const auto finished = anchor.finish();
    CHECK(finished.message.sequence == prior.message.sequence + 1);
    CHECK(finished.failure.code == RunFailureCode::none);
    CHECK(finished.message.slots == prior.message.slots);
    CHECK(!anchor.refusal());
    CHECK(external_storage(anchor).complete()); // finished is inline state
    CHECK(!anchor.refusal()); // FINISH may end host service; observation needs no RPC.
}

// Actual opaque/untracked descendants drive three different unsupported masks.
// The known vector is a lower bound even when the configured budget is UINT64_MAX.
struct UnknownState : RuntimeContextEvaluator {
    struct Opaque {
        std::vector<double> values;
        explicit Opaque(std::size_t count) : values(count) {}
    };
    std::vector<unsigned char> known;
    std::optional<Opaque> opaque;
    std::optional<std::deque<double>> untracked;
    std::uint64_t calls = 0;
    UnknownState(std::size_t bytes, unsigned mask) : known(bytes) {
        if (mask & 1) opaque.emplace(bytes + 1);
        if (mask & 2) untracked.emplace(bytes + 2);
    }
    void consume(RuntimeContextService&, RuntimeContextHandle,
                 const RuntimeContextEvent&) override { ++calls; }
    std::uint64_t storage_bytes() const noexcept override {
        OwnedStorage out;
        observe_owned_storage(out);
        return out.usage().storage_bytes();
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept override {
        out.metadata(sizeof(*this));
        observe_owned_members(out, known, opaque, untracked);
    }
};

// Arithmetic overflow fixture: the observation API, not an allocation of this
// size, produces a saturated lower bound and an explicit overflow witness.
struct OverflowState : RuntimeContextEvaluator {
    void consume(RuntimeContextService&, RuntimeContextHandle,
                 const RuntimeContextEvent&) override {}
    std::uint64_t storage_bytes() const noexcept override {
        return std::numeric_limits<std::uint64_t>::max();
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept override {
        out.metadata(sizeof(*this));
        out.payload(out.product(std::numeric_limits<std::uint64_t>::max(), 2));
    }
};

void incomplete_failures_carry_real_lower_bounds() {
    const auto exercise = [](const RuntimeContextService::Factory& factory,
                             std::uint64_t flags, bool overflow, std::size_t known_bytes) {
        const Bar input = bar(0);
        RuntimeBatchLease lease(&input, 1);
        RuntimeContextService unlimited;
        CHECK(!RuntimeHistoryOptions{}.runtimeHistoryBudgetBytes);
        unlimited.bind_batch(lease);
        unlimited.accept_batch(1);
        const RuntimeContextKey key{506, "unknown storage", {}};
        const auto h = unlimited.get_or_create(key, factory);
        unlimited.advance(h, 1); // Default NONE still publishes incomplete owners.
        const auto expected = unlimited.usage().owned_storage;
        CHECK(expected.unsupported == flags && expected.overflow == overflow);
        CHECK(expected.resident_payload_bytes >= known_bytes);
        CHECK(!expected.complete());
        (void)unlimited.published(h);
        for (const auto bound : {std::uint64_t{0}, std::uint64_t{4093},
                                 std::numeric_limits<std::uint64_t>::max()}) {
            RuntimeContextService limited;
            limited.configure({0, bound});
            limited.bind_batch(lease);
            limited.accept_batch(1);
            bool refused = false;
            try { limited.get_or_create(key, factory); }
            catch (const RuntimeStorageObservationIncomplete& error) {
                refused = true;
                check_observation_failure(error, expected);
            }
            CHECK(refused);
            CHECK(limited.usage().contexts == 0);
            CHECK(limited.accepted_frontier() == 1);
            CHECK(limited.usage().owned_storage.complete()); // Failed candidate was destroyed.
        }
    };
    const std::array<std::size_t, 3> sizes{19, 97, 521};
    for (unsigned mask = 1; mask <= sizes.size(); ++mask) {
        const auto bytes = sizes[mask - 1];
        const auto flags = ((mask & 1) ? static_cast<std::uint64_t>(
            OwnedStorageUnsupported::opaque_type) : 0)
            | ((mask & 2) ? static_cast<std::uint64_t>(
                OwnedStorageUnsupported::allocator) : 0);
        exercise([=] { return std::make_unique<UnknownState>(bytes, mask); },
                 flags, false, bytes);
    }
    exercise([] { return std::make_unique<OverflowState>(); }, 0, true, 0);
}

void incomplete_overflow_and_allocation_failures_remain_observable() {
    struct Opaque { std::vector<double> owned; };
    Opaque opaque{{1.0, 2.0}};
    CHECK(!external_storage(opaque).complete());
    CHECK(external_storage(opaque).storage_bytes() == std::numeric_limits<std::uint64_t>::max());
    std::deque<double> untracked(3);
    CHECK(external_storage(untracked).unsupported
          & static_cast<std::uint64_t>(OwnedStorageUnsupported::allocator));
    {
        OwnedStorage out;
        out.metadata(std::numeric_limits<std::uint64_t>::max());
        out.payload(1);
        CHECK(out.usage().overflow && !out.usage().complete());
        CHECK(out.usage().storage_bytes() == std::numeric_limits<std::uint64_t>::max());
    }
    {
        OwnedStorage out;
        out.payload(out.product(std::numeric_limits<std::uint64_t>::max(), 2));
        CHECK(out.usage().overflow);
    }
    Series<double> values(1025);
    values.push(7.0);
    const auto before = values[0];
    OwnedStorage refused;
    global_allocation::refusing = true;
    values.observe_owned_storage(refused);
    global_allocation::refusing = false;
    CHECK(!refused.usage().complete());
    CHECK(refused.usage().unsupported
          & static_cast<std::uint64_t>(OwnedStorageUnsupported::observation_allocation));
    CHECK(values[0] == before);

    struct Legacy : RuntimeContextEvaluator {
        void consume(RuntimeContextService&, RuntimeContextHandle,
                     const RuntimeContextEvent&) override {}
        std::uint64_t storage_bytes() const noexcept override { return sizeof(*this); }
    };
    RuntimeContextService unlimited;
    unlimited.begin_live();
    const auto h = unlimited.get_or_create({505, "opaque", {}},
        [] { return std::make_unique<Legacy>(); });
    unlimited.append_live(bar(0));
    unlimited.advance(h, 1); // default NONE still executes
    CHECK(!unlimited.usage().owned_storage.complete());
    RuntimeContextService limited;
    limited.configure({0, std::numeric_limits<std::uint64_t>::max()});
    limited.begin_live();
    CHECK(throws<RuntimeStorageObservationIncomplete>([&] {
        limited.get_or_create({505, "opaque", {}},
            [] { return std::make_unique<Legacy>(); });
    }));
    CHECK(limited.usage().contexts == 0);
}

// Factory-minted failure args own one real allocation domain: the JSON string
// object and the shared control are OwnedAllocator requests beside a counted
// ledger and records; the buffer is the string's own capacity. Oracles are the
// independent new/delete census and JSON written from these authored inputs.
void factory_failure_args_own_their_json_domain() {
    AllocationCensus census;
    std::size_t previous = 0;
    for (const std::size_t width : {std::size_t{1}, std::size_t{97}, std::size_t{1031}}) {
        std::string call(width, 'c');
        for (std::size_t i = 0; i < call.size(); ++i)
            call[i] = static_cast<char>('a' + (i * 7 + width) % 26);
        const RunFailureArgs args{{"function", "request.financial"}, {"call", call}, {"line", 7}};
        const std::string expected = "{\"call\":\"" + call
            + "\",\"function\":\"request.financial\",\"line\":7}";
        const std::string foreign(width + 64, 'f');
        const auto before = global_allocation::live_bytes;
        const auto events = global_allocation::allocations;
        auto value = make_run_failure(RunFailureCode::no_data_request, args);
        const auto requests = global_allocation::live_bytes - before;
        const auto request_events = global_allocation::allocations - events;
        CHECK(value.code == RunFailureCode::no_data_request);
        CHECK(value.args && *value.args == expected);
        if (!value.args || *value.args != expected) return;
        CHECK(requests > previous && request_events >= 5);
        previous = requests;
        errno = ERANGE;
        const auto observed = external_storage(value);
        CHECK(errno == ERANGE);
        CHECK(observed.complete() && !observed.overflow && observed.backing_bytes == 0);
        CHECK(observed.resident_bytes() == requests);
        CHECK(observed.resident_payload_bytes >= sizeof(std::string) + value.args->capacity() + 1);
        CHECK(global_allocation::live_bytes == before + requests);
        auto copy = value;
        check_same_storage(external_storage(value, copy, value), observed);
        auto moved = std::move(copy);
        CHECK(external_storage(copy).complete() && external_storage(copy).resident_bytes() == 0);
        check_same_storage(external_storage(moved), observed);
        {
            const RunFailureValue null_alias{
                value.code, std::shared_ptr<const std::string>(value.args, nullptr)};
            CHECK(!null_alias.args && null_alias.args.use_count() > 0);
            check_same_storage(external_storage(null_alias), observed);
            const RunFailureValue foreign_alias{
                value.code, std::shared_ptr<const std::string>(value.args, &foreign)};
            const auto aliased = external_storage(foreign_alias);
            CHECK(!aliased.complete() && aliased.resident_bytes() == requests);
            CHECK(aliased.unsupported
                  == static_cast<std::uint64_t>(OwnedStorageUnsupported::opaque_type));
            const RunFailureValue unowned{value.code, std::shared_ptr<const std::string>(
                std::shared_ptr<const std::string>(), &foreign)};
            const auto alias_only = external_storage(unowned);
            CHECK(!alias_only.complete() && alias_only.resident_bytes() == 0);
            CHECK(alias_only.unsupported
                  == static_cast<std::uint64_t>(OwnedStorageUnsupported::opaque_type));
            const RunFailureValue unknown{value.code, std::make_shared<const std::string>(expected)};
            const auto opaque = external_storage(unknown);
            CHECK(!opaque.complete() && opaque.resident_bytes() == 0);
            CHECK(opaque.unsupported
                  == (static_cast<std::uint64_t>(OwnedStorageUnsupported::allocator)
                      | static_cast<std::uint64_t>(OwnedStorageUnsupported::opaque_type)));
            const auto joined = external_storage(unknown, value, foreign_alias);
            CHECK(!joined.complete() && joined.resident_bytes() == requests);
            const RunFailureValue empty{};
            CHECK(external_storage(empty).complete() && external_storage(empty).resident_bytes() == 0);
            const auto code_only = make_run_failure(RunFailureCode::strategy_runtime_error, {});
            CHECK(code_only.code == RunFailureCode::strategy_runtime_error && !code_only.args);
            CHECK(code_only.args.use_count() == 0 && external_storage(code_only).complete());
        }
        {
            OwnedStorage refused;
            const auto refusals = global_allocation::refused;
            global_allocation::refusing = true;
            errno = EDOM;
            value.observe_owned_storage(refused);
            global_allocation::refusing = false;
            CHECK(errno == EDOM && global_allocation::refused > refusals);
            CHECK(refused.usage().unsupported & static_cast<std::uint64_t>(
                OwnedStorageUnsupported::observation_allocation));
        }
        std::weak_ptr<const std::string> weak = value.args;
        value = RunFailureValue{};
        moved = RunFailureValue{};
        CHECK(weak.expired());
        CHECK(global_allocation::live_bytes > before);            // control + ledger
        CHECK(global_allocation::live_bytes < before + requests); // object + buffer freed
        weak.reset();
        CHECK(global_allocation::live_bytes == before);           // no owning cycle
        for (std::size_t fail_at = 0; fail_at < request_events; ++fail_at) {
            global_allocation::fail_after = fail_at;
            auto failed = make_run_failure(RunFailureCode::no_data_request, args);
            global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
            CHECK(failed.code == RunFailureCode::out_of_memory);
            CHECK(!failed.args && failed.args.use_count() == 0);
            CHECK(global_allocation::live_bytes == before);
        }
        global_allocation::fail_after = request_events;
        auto boundary = make_run_failure(RunFailureCode::no_data_request, args);
        global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
        CHECK(boundary.args && *boundary.args == expected);
        CHECK(global_allocation::live_bytes == before + requests);
        boundary = RunFailureValue{};
        CHECK(global_allocation::live_bytes == before);
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--managed-scratch-observation") == 0) {
        managed_scratch_observation_traverses_endpoint_and_preserves_last_close();
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 5 && std::strcmp(argv[1], "--managed-quota-endpoint-observation") == 0) {
        std::array<int, 3> fds{};
        for (std::size_t i = 0; i < fds.size(); ++i) {
            char* end = nullptr;
            const long value = std::strtol(argv[i + 2], &end, 10);
            const bool valid = end != argv[i + 2] && !*end && value >= 0
                && value <= std::numeric_limits<int>::max();
            CHECK(valid);
            if (!valid) return EXIT_FAILURE;
            fds[i] = static_cast<int>(value);
        }
        managed_quota_endpoint_graph(fds);
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    CHECK(argc == 1);
    batch_lease_replay_release_and_generation();
    owning_source_survives_entries_and_releases();
    batch_storage_is_independent_of_caller_history_size();
    factory_cycles_do_not_publish_a_partial_context();
    nested_discovery_keeps_references_and_keys_separate();
    exact_budget_and_default_none();
    spill_and_fixed_resident_bookkeeping();
    source_context_metadata_restores_across_nested_callbacks();
    native_host_batch_stream_and_static_budget_controls();
    series_and_container_owned_allocation_census();
    aggregator_native_capacity_copy_and_release_census();
    multi_day_shared_params_and_sources_census();
    multi_day_generator_answers_copy_restart_and_release_census();
    multi_day_allocation_failures_and_untracked_owners();
    unsupported_period_source_is_incomplete_and_unqueried();
    nested_capacity_and_first_member_identity_census();
    ta_window_length_and_saved_state_census();
    live_nested_shared_growth_and_shrink_census();
    real_owned_growth_enforces_budget_before_publication();
    plain_scratch_directory_fd_lifecycle_census();
    scratch_extent_and_observer_failures_are_incomplete();
    scratch_backing_and_cache_observations_are_separate();
    scratch_arena_keeps_known_directory_and_unknown_borrowed_owner();
    scratch_slab_and_custom_descendants_stay_incomplete();
    unmanaged_quota_hooks_own_no_endpoint();
    incomplete_failures_carry_real_lower_bounds();
    incomplete_overflow_and_allocation_failures_remain_observable();
    factory_failure_args_own_their_json_domain();
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
