// Configured populations are compared at 32 and 128 bars.
// Expectations are derived from the inputs and implementation.
// Checks cover only storage reachable and observable through these fixtures.
// Parse SessionKey before map.hpp exposes ScriptString comparison overloads.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/map.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/timeframe.hpp>

#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
namespace ps = pineforge::source;
using Host = ps::PineStrategyHost;
using Scheduler = ps::PineScheduler;
using Consumer = NativeExecutionConsumer;
using BoundEma = ps::FirstCallBound<ta::EMA>;
using InnerMap = PineMap<int, int>;
using HandleMap = PineMap<int, InnerMap>;

// The repository's explicit-instantiation access pattern. These pointers read
// the actual owners; no copied class definition or product friend is needed.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
#define MEM_MEMBER(tag, owner, field)                \
    struct tag { friend auto access(tag); };        \
    template struct Access<tag, &owner::field>

MEM_MEMBER(BoundObject, BoundEma, object_);
MEM_MEMBER(AuxTimeframe, Host, aux_security_input_tf_);
MEM_MEMBER(HostScheduler, Host, scheduler_);
MEM_MEMBER(InputCursor, Scheduler, input_script_cursor);
MEM_MEMBER(ConsumedCursor, Scheduler, consumed_script_cursor_);
MEM_MEMBER(CursorScript, ScriptBucketCursor, script_tf_);
MEM_MEMBER(CursorInput, ScriptBucketCursor, input_tf_);
MEM_MEMBER(CursorTimezone, ScriptBucketCursor, timezone_);
MEM_MEMBER(CursorSession, ScriptBucketCursor, session_);
MEM_MEMBER(CursorAggregator, ScriptBucketCursor, aggregator_);
MEM_MEMBER(AggregatorTimezone, TimeframeAggregator, anchor_tz_);
MEM_MEMBER(AggregatorSession, TimeframeAggregator, anchor_session_);
MEM_MEMBER(AggregatorStamps, TimeframeAggregator, native_stamps_);
MEM_MEMBER(AggregatorGroups, TimeframeAggregator, native_group_open_);
MEM_MEMBER(AggregatorSource, TimeframeAggregator, native_source_);
MEM_MEMBER(AggregatorLate, TimeframeAggregator, native_late_opens_);
MEM_MEMBER(Subscriptions, Consumer, subscriptions_);
MEM_MEMBER(PendingViews, Consumer, pending_bar_views_);
MEM_MEMBER(ActiveViews, Consumer, active_bar_views_);
MEM_MEMBER(StageViews, Host, stage_symbol_feed_views);
MEM_MEMBER(DayPartition, BacktestEngine, chart_day_partition_);
MEM_MEMBER(TraceIndex, BacktestEngine, trace_name_index_);
MEM_MEMBER(NativeFeeds, BacktestEngine, native_security_feeds_);
MEM_MEMBER(Inputs, BacktestEngine, inputs_);
MEM_MEMBER(Metadata, BacktestEngine, syminfo_metadata_);
MEM_MEMBER(FxRates, BacktestEngine, account_currency_fx_rates_);
MEM_MEMBER(MapStorage, HandleMap, storage_);
#undef MEM_MEMBER

std::size_t checks = 0;
std::size_t failures = 0;

#define ROW_CHECK(row, expression)                                              \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expression)) {                                                   \
            ++failures;                                                        \
            std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n",                     \
                         row, __FILE__, __LINE__, #expression);                 \
        }                                                                      \
    } while (false)

constexpr int kBars = 32;
constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kDay = 24 * 60 * kMinute;
constexpr std::int64_t kStart = 1704067200000LL; // Monday, 2024-01-01 UTC

std::vector<Bar> tape(int n, std::int64_t step = kMinute) {
    std::vector<Bar> result;
    result.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        result.push_back({100.0, 101.0, 99.0, 100.0, 1.0, kStart + i * step});
    return result;
}

// Optional storage and sequence storage have different count interfaces. The
// sequence overload also accepts a one-slot optional replaced by an
// accumulating sequence without turning a runtime proof into a compile error.
template<class T> std::size_t slots(const std::optional<T>& value) {
    return value.has_value() ? 1U : 0U;
}
template<class T, class Allocator>
std::size_t slots(const std::vector<T, Allocator>& value) {
    return value.size();
}

// A conservative layout allowance for the supported standard libraries: one
// doubling of caller text plus terminator, and one string object's worth of
// inline-buffer/alignment slack. This is arithmetic, not a measured constant.
std::size_t text_allowance(std::size_t length) {
    return 2 * (length + 1) + sizeof(std::string);
}

class TradingHost : public Host {
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

    void on_source_bar(const Bar&) override { trade_bar(); }

    void trade_bar() {
        ++seen;
        const int i = pine_bar_index();
        // Repeated real market submit, next-open fill, close and close fill.
        if (i % 8 == 0) strategy_entry("IN-position", true);
        if (i % 8 == 3) strategy_close("IN-position");
    }

    int seen = 0;
};

void run(TradingHost& host, const std::vector<Bar>& bars, const char* row,
         const char* input_tf = "1", const char* script_tf = "1",
         int expected_script_bars = -1) {
    host.seen = 0;
    bool returned = false;
    try {
        host.run(bars.data(), static_cast<int>(bars.size()), input_tf, script_tf,
                 false, 4, MagnifierDistribution::ENDPOINTS);
        returned = true;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "DETAIL [%s] run exception: %s\n", row, error.what());
    } catch (...) {
        std::fprintf(stderr, "DETAIL [%s] non-standard run exception\n", row);
    }
    // An early run refusal must not hide the later size assertion.
    ROW_CHECK(row, returned);
    ROW_CHECK(row, host.last_error().empty());
    if (!host.last_error().empty())
        std::fprintf(stderr, "DETAIL [%s] %s\n", row, host.last_error().c_str());
    const int expected = expected_script_bars < 0
        ? static_cast<int>(bars.size()) : expected_script_bars;
    ROW_CHECK(row, host.seen == expected);
    ROW_CHECK(row, host.trade_count() > 0); // stimulus control, not the proof
}

class BoundHost final : public TradingHost {
public:
    BoundEma ema;
    void on_source_bar(const Bar& bar) override {
        if (pine_bar_index() == 0) ema = BoundEma{};
        (void)ema.compute([] { return ta::EMA(7); }, bar.close);
        trade_bar();
    }
};

void first_call_object(const char* row) {
    // One configured TA call site constructs one EMA. EMA has scalar state,
    // so this instantiation has no unexamined dynamic T payload. This is an
    // object-count proof, not a universal byte bound for arbitrary template T.
    static_assert(std::is_trivially_copyable_v<ta::EMA>, "EMA owns scalar state");
    BoundHost host;
    run(host, tape(kBars), row);
    const auto short_count = slots(host.ema.*access(BoundObject{}));
    run(host, tape(4 * kBars), row);
    const auto long_count = slots(host.ema.*access(BoundObject{}));
    const std::size_t configured_call_sites = 1;
    ROW_CHECK(row, short_count == configured_call_sites
              && long_count == configured_call_sites && short_count == long_count);
}

void auxiliary_timeframe(const char* row) {
    TradingHost host;
    // The SAME 512 one-minute auxiliary bars cover both primary lengths:
    // 128 primary bars * 4 minutes each. Only the stored timeframe text is
    // this check's container; feed bars are checked separately.
    const auto auxiliary = tape(4 * 4 * kBars);
    const std::string configured = "1";
    for (int replacement = 0; replacement < 4; ++replacement)
        ROW_CHECK(row, host.set_aux_security_feed(auxiliary.data(),
                  static_cast<int>(auxiliary.size()), configured));
    run(host, tape(kBars, 4 * kMinute), row, "4", "4");
    const std::string short_text = host.*access(AuxTimeframe{});
    const auto short_capacity = (host.*access(AuxTimeframe{})).capacity() + 1;
    run(host, tape(4 * kBars, 4 * kMinute), row, "4", "4");
    const auto& long_text = host.*access(AuxTimeframe{});
    // One installed timeframe, exactly the caller's one ASCII byte.
    ROW_CHECK(row, short_text.size() == configured.size()
              && long_text.size() == configured.size()
              && short_text.size() == long_text.size()
              && short_text == configured && long_text == configured
              && short_capacity == long_text.capacity() + 1
              && short_capacity <= text_allowance(configured.size()));
    ROW_CHECK(row, host.set_aux_security_feed(nullptr, 0, configured));
    ROW_CHECK(row, (host.*access(AuxTimeframe{})).size() == 0);
}

struct CursorStorage {
    std::size_t objects = 0;
    std::array<std::string, 6> text;
    std::array<std::size_t, 4> installed{};
    std::size_t text_capacity = 0;
    std::size_t partition_capacity = 0;
    std::size_t text_bytes() const {
        std::size_t result = 0;
        for (const auto& value : text) result += value.size();
        return result;
    }
};

CursorStorage cursor_storage(const std::unique_ptr<ScriptBucketCursor>& cursor) {
    CursorStorage result;
    result.objects = cursor ? 1U : 0U;
    if (!cursor) return result;
    const auto& aggregate = (*cursor).*access(CursorAggregator{});
    const std::array<const std::string*, 6> fields{{
        &((*cursor).*access(CursorScript{})), &((*cursor).*access(CursorInput{})),
        &((*cursor).*access(CursorTimezone{})), &((*cursor).*access(CursorSession{})),
        &(aggregate.*access(AggregatorTimezone{})), &(aggregate.*access(AggregatorSession{}))}};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        result.text[i] = *fields[i];
        result.text_capacity += fields[i]->capacity() + 1;
    }
    result.installed = {{(aggregate.*access(AggregatorStamps{})).size(),
                         (aggregate.*access(AggregatorGroups{})).size(),
                         (aggregate.*access(AggregatorLate{})).size(),
                         (aggregate.*access(AggregatorSource{})) ? 1U : 0U}};
    result.partition_capacity = (aggregate.*access(AggregatorStamps{})).capacity()
        + (aggregate.*access(AggregatorGroups{})).capacity()
        + (aggregate.*access(AggregatorLate{})).capacity();
    return result;
}

class CursorHost final : public TradingHost {
public:
    void on_source_bar(const Bar&) override {
        trade_bar();
        // Exercise the consumed-prefix fold as well as input completion.
        (void)broker_state_hash();
    }
};

void scheduler_cursor(const char* row, bool consumed) {
    CursorHost host;
    const auto sample = [&] {
        const auto& scheduler = host.*access(HostScheduler{});
        return cursor_storage(consumed ? scheduler.*access(ConsumedCursor{})
                                       : scheduler.*access(InputCursor{}));
    };
    run(host, tape(kBars), row, "1", "4", kBars / 4);
    const CursorStorage short_run = sample();
    run(host, tape(4 * kBars), row, "1", "4", kBars);
    const CursorStorage long_run = sample();
    // A 4:1 aggregation needs one cursor. Four copied spec strings plus the
    // aggregator's timezone/session copies contain 1+1+3+4+3+4 = 16 bytes.
    // No native stamp/group/late-open/source setter is configured for it.
    const std::array<std::string, 6> text{{"4", "1", "UTC", "24x7", "UTC", "24x7"}};
    const std::array<std::size_t, 4> no_partition{{0, 0, 0, 0}};
    const std::size_t configured_bytes = 1 + 1 + 3 + 4 + 3 + 4;
    std::size_t capacity_allowance = 0;
    for (const auto& field : text) capacity_allowance += text_allowance(field.size());
    ROW_CHECK(row, short_run.objects == 1 && long_run.objects == 1
              && short_run.objects == long_run.objects
              && short_run.text_bytes() == configured_bytes
              && long_run.text_bytes() == configured_bytes
              && short_run.text == text && long_run.text == text
              && short_run.installed == no_partition && long_run.installed == no_partition
              && short_run.partition_capacity == 0 && long_run.partition_capacity == 0
              && short_run.text_capacity == long_run.text_capacity
              && short_run.text_capacity <= capacity_allowance);
}

class SubscriptionHost final : public TradingHost {
public:
    const std::array<std::string, 3> declared{{"2", "4", "8"}};
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        for (std::size_t i = 0; i < declared.size(); ++i)
            register_security_eval(static_cast<int>(i), declared[i], "1", false, false);
    }
};

void subscriptions(const char* row) {
    SubscriptionHost host;
    const auto sample = [&] {
        const auto& actual = Consumer::bound(host).*access(Subscriptions{});
        std::vector<std::string> result;
        for (const auto& subscription : actual) result.push_back(subscription.tf_literal);
        return result;
    };
    run(host, tape(kBars), row);
    const auto short_run = sample();
    run(host, tape(4 * kBars), row);
    const auto long_run = sample();
    // Three distinct declarations, one native subscription per declaration.
    const std::vector<std::string> model(host.declared.begin(), host.declared.end());
    ROW_CHECK(row, short_run.size() == model.size() && long_run.size() == model.size()
              && short_run.size() == long_run.size() && short_run == model && long_run == model);
}

struct FeedViewsSnapshot {
    std::size_t pending = 0;
    std::size_t active = 0;
    std::vector<std::size_t> rows;
    bool detached = true;
};

FeedViewsSnapshot feed_views(TradingHost& host) {
    const auto& consumer = Consumer::bound(host);
    const auto& pending = consumer.*access(PendingViews{});
    const auto& active = consumer.*access(ActiveViews{});
    FeedViewsSnapshot result;
    result.pending = pending.descriptors.size();
    result.active = active.descriptors.size();
    for (const auto& descriptor : active.descriptors) {
        result.rows.push_back(descriptor ? descriptor->bars.size : 0);
        result.detached &= !descriptor
            || (descriptor->bars.data == nullptr && descriptor->close_ms == nullptr);
    }
    return result;
}

void bar_view_descriptors(const char* row) {
    TradingHost host;
    // Three installed feeds each have eight caller-owned rows. Their backing
    // stays alive through both historical runs; primary length is independent.
    const std::array<std::string, 3> keys{{"TEST:ALPHA", "TEST:BETA", "TEST:GAMMA"}};
    const auto feed = tape(8);
    std::vector<std::int64_t> closes;
    for (const auto& bar : feed) closes.push_back(bar.timestamp + kMinute);
    for (const auto& key : keys)
        ROW_CHECK(row, host.set_symbol_feed_borrowed(key, "1", feed.data(),
                  closes.data(), static_cast<int>(feed.size())));
    // Drive the production staging method repeatedly, as well as the call
    // made by prepare_native_begin: a staged replacement must retire the old
    // descriptor vector even before any run consumes it.
    for (int replacement = 0; replacement < 4; ++replacement) {
        (host.*access(StageViews{}))();
        ROW_CHECK(row, (Consumer::bound(host).*access(PendingViews{})).descriptors.size()
                  == keys.size());
    }
    run(host, tape(kBars), row);
    const auto short_run = feed_views(host);
    run(host, tape(4 * kBars), row);
    const auto long_run = feed_views(host);
    const std::vector<std::size_t> rows(keys.size(), feed.size());
    // One descriptor per installed feed; detach retains counts, with no
    // staged descriptors and no live borrowed data pointers after either run.
    ROW_CHECK(row, short_run.active == keys.size() && long_run.active == keys.size()
              && short_run.active == long_run.active
              && short_run.pending == 0 && long_run.pending == 0
              && short_run.rows == rows && long_run.rows == rows
              && short_run.detached && long_run.detached);
}

struct DailyConfiguration {
    // Four supplied daily stamps. Jan 2 opens one hour late and Jan 4 two
    // hours late; both primary lengths stay on Jan 1, so neither supplies a
    // bar that contradicts those two late opens. The configured feed itself
    // is identical at both primary lengths.
    std::vector<std::int64_t> stamps{kStart, kStart + kDay + 60 * kMinute,
        kStart + 2 * kDay, kStart + 3 * kDay + 120 * kMinute};
    std::vector<std::int64_t> late{stamps[1], stamps[3]};
    std::vector<Bar> bars() const {
        std::vector<Bar> result;
        for (const auto stamp : stamps)
            result.push_back({100.0, 101.0, 99.0, 100.0, 1.0, stamp});
        return result;
    }
};

class DayHost final : public TradingHost {
public:
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        // A ratio aggregator consumes the daily feed's late-open grid.
        register_security_eval(0, "4", "1", false, false);
    }
    std::vector<std::int64_t> late_opens() const {
        if (security_eval_states_.size() != 1) return {};
        return security_eval_states_[0].aggregator.*access(AggregatorLate{});
    }
    bool replace_late_opens(const std::vector<std::int64_t>& opens) {
        if (security_eval_states_.size() != 1) return false;
        security_eval_states_[0].aggregator.set_native_day_opens(opens);
        return true;
    }
};

std::array<std::size_t, 5> partition_counts(const TradingHost& host) {
    const auto& partition = host.*access(DayPartition{});
    return {{partition.stamps.size(), partition.trade_day.size(),
             partition.week_open.size(), partition.month_open.size(),
             partition.late_opens.size()}};
}

void day_partition(const char* row) {
    TradingHost host;
    const DailyConfiguration configured;
    const auto daily = configured.bars();
    ROW_CHECK(row, host.set_native_security_feed("D", daily.data(),
              static_cast<int>(daily.size())));
    run(host, tape(kBars), row);
    const auto short_counts = partition_counts(host);
    const auto short_stamps = (host.*access(DayPartition{})).stamps;
    const auto short_late = (host.*access(DayPartition{})).late_opens;
    run(host, tape(4 * kBars), row);
    const auto long_counts = partition_counts(host);
    const auto& actual = host.*access(DayPartition{});
    // Each of the four supplied stamps contributes one element to each
    // parallel column; exactly the two explicitly late stamps enter late_opens.
    // The engine stores three columns as FeedProjection.
    const std::array<std::size_t, 5> model{{configured.stamps.size(),
        configured.stamps.size(), configured.stamps.size(),
        configured.stamps.size(), configured.late.size()}};
    ROW_CHECK(row, short_counts == model && long_counts == model
              && short_counts == long_counts && short_stamps == configured.stamps
              && actual.stamps == configured.stamps && short_late == configured.late
              && actual.late_opens == configured.late);
}

void aggregator_late_opens(const char* row) {
    DayHost host;
    const DailyConfiguration configured;
    const auto daily = configured.bars();
    ROW_CHECK(row, host.set_native_security_feed("D", daily.data(),
              static_cast<int>(daily.size())));
    run(host, tape(kBars), row);
    // Replace the actual engine-owned aggregator's input through its real
    // setter, then read its container. Never write the vector from the test.
    for (int replacement = 0; replacement < 4; ++replacement) {
        ROW_CHECK(row, host.replace_late_opens(configured.late));
        ROW_CHECK(row, host.late_opens().size() == configured.late.size());
    }
    const auto short_run = host.late_opens();
    run(host, tape(4 * kBars), row);
    for (int replacement = 0; replacement < 4; ++replacement) {
        ROW_CHECK(row, host.replace_late_opens(configured.late));
        ROW_CHECK(row, host.late_opens().size() == configured.late.size());
    }
    const auto long_run = host.late_opens();
    // Two late stamps in the configured four-day feed, independent of bars.
    ROW_CHECK(row, short_run.size() == configured.late.size()
              && long_run.size() == configured.late.size()
              && short_run.size() == long_run.size()
              && short_run == configured.late && long_run == configured.late);
}

class TraceHost final : public TradingHost {
public:
    const std::array<std::string, 3> labels{{"configured-open", "configured-close", "range"}};
    TraceHost() { set_trace_enabled(true); }
    void on_source_bar(const Bar& bar) override {
        for (const auto& label : labels) trace(label, bar.close);
        trade_bar();
    }
};

void trace_index(const char* row) {
    TraceHost host;
    run(host, tape(kBars), row);
    const auto short_run = host.*access(TraceIndex{});
    run(host, tape(4 * kBars), row);
    const auto& long_run = host.*access(TraceIndex{});
    // IDs follow first occurrence: 0, 1, 2, one node per distinct configured
    // label although each label is emitted on every primary bar.
    std::unordered_map<std::string, std::int32_t> model;
    for (std::size_t i = 0; i < host.labels.size(); ++i)
        model.emplace(host.labels[i], static_cast<std::int32_t>(i));
    ROW_CHECK(row, short_run.size() == model.size() && long_run.size() == model.size()
              && short_run.size() == long_run.size() && short_run == model && long_run == model);
}

void native_security_feeds(const char* row) {
    TradingHost host;
    const std::array<std::string, 3> timeframes{{"D", "W", "M"}};
    const auto supplied = tape(4, kDay);
    for (int replacement = 0; replacement < 4; ++replacement)
        for (const auto& tf : timeframes)
            ROW_CHECK(row, host.set_native_security_feed(tf, supplied.data(),
                      static_cast<int>(supplied.size())));
    const auto sample = [&] {
        std::vector<std::pair<std::string, std::size_t>> result;
        for (const auto& feed : host.*access(NativeFeeds{}))
            result.emplace_back(feed.tf, feed.bars.size());
        return result;
    };
    run(host, tape(kBars), row);
    const auto short_run = sample();
    run(host, tape(4 * kBars), row);
    const auto long_run = sample();
    std::vector<std::pair<std::string, std::size_t>> model;
    for (const auto& tf : timeframes) model.emplace_back(tf, supplied.size());
    // Three configured timeframes; replacement of a timeframe is not a new
    // feed. The four rows in each feed also remain the caller's four rows.
    ROW_CHECK(row, short_run.size() == timeframes.size() && long_run.size() == timeframes.size()
              && short_run.size() == long_run.size() && short_run == model && long_run == model);
}

void inputs(const char* row) {
    TradingHost host;
    const std::unordered_map<std::string, std::string> model{
        {"input-length", "7"}, {"input-mode", "fixed"}, {"input-side", "long"}};
    host.set_input("prior-only-a", "retired");
    host.set_input("prior-only-b", "retired");
    host.clear_inputs();
    for (int replacement = 0; replacement < 4; ++replacement)
        for (const auto& input : model) host.set_input(input.first, input.second);
    run(host, tape(kBars), row);
    const auto short_run = host.*access(Inputs{});
    run(host, tape(4 * kBars), row);
    const auto& long_run = host.*access(Inputs{});
    // Two retired keys contribute zero; the configured three unique keys
    // contribute three, independent of repeated assignments and primary bars.
    ROW_CHECK(row, short_run.size() == model.size() && long_run.size() == model.size()
              && short_run.size() == long_run.size() && short_run == model && long_run == model);
}

void metadata(const char* row) {
    TradingHost host;
    const std::unordered_map<std::string, double> model{
        {"shares_outstanding_total", 1000.0}, {"target_price_high", 150.0},
        {"target_price_low", 75.0}};
    for (int replacement = 0; replacement < 4; ++replacement)
        for (const auto& field : model) host.set_syminfo_metadata(field.first, field.second);
    run(host, tape(kBars), row);
    const auto short_run = host.*access(Metadata{});
    run(host, tape(4 * kBars), row);
    const auto& long_run = host.*access(Metadata{});
    // Three distinct caller keys, with repeated writes to the same keys.
    ROW_CHECK(row, short_run.size() == model.size() && long_run.size() == model.size()
              && short_run.size() == long_run.size() && short_run == model && long_run == model);
}

void fx_rates(const char* row) {
    TradingHost host;
    const std::array<std::int64_t, 4> timestamps{{kStart, kStart + 7 * kMinute,
        kStart + 19 * kMinute, kStart + 31 * kMinute}};
    const std::vector<double> model{1.0, 1.25, 0.5, 2.0};
    for (int replacement = 0; replacement < 4; ++replacement)
        ROW_CHECK(row, host.set_account_currency_fx_series(timestamps.data(),
                  model.data(), static_cast<int>(model.size())));
    run(host, tape(kBars), row);
    const auto short_run = host.*access(FxRates{});
    run(host, tape(4 * kBars), row);
    const auto& long_run = host.*access(FxRates{});
    // One rate per configured timestamp = four rates. Passing later bars
    // changes the effective-rate cursor, not the retained provider population.
    ROW_CHECK(row, short_run.size() == timestamps.size() && long_run.size() == timestamps.size()
              && short_run.size() == long_run.size() && short_run == model && long_run == model);
}

class MapHost final : public TradingHost {
public:
    HandleMap values;
    InnerMap payload;
    std::array<int, 4> live_keys{{0, 1, 2, 3}};
    int next_key = 4;

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            values = HandleMap::new_();
            payload = InnerMap::new_();
            (void)payload.put(7, 123);
            live_keys = {{0, 1, 2, 3}};
            next_key = static_cast<int>(live_keys.size());
            for (const int key : live_keys) (void)values.put(key, payload);
        }
        // Four configured slots, one retiring and one entering per bar.
        // All values are genuine map handles and select kHeapValue storage.
        const auto slot = static_cast<std::size_t>(i) % live_keys.size();
        (void)values.remove(live_keys[slot]);
        live_keys[slot] = next_key++;
        (void)values.put(live_keys[slot], payload);
        trade_bar();
    }

    std::size_t heap_slots() const {
        const auto& storage = values.*access(MapStorage{});
        return storage ? storage->heap_values.size() : 0;
    }
    bool has_model() const {
        if (values.is_na() || values.size() != static_cast<int>(live_keys.size())) return false;
        for (const int key : live_keys) {
            const auto value = values.get(key);
            if (value.is_na() || value.get(7) != 123) return false;
        }
        return true;
    }
};

void handle_map_slots(const char* row) {
    MapHost host;
    run(host, tape(kBars), row);
    const auto short_count = host.heap_slots();
    ROW_CHECK(row, host.has_model());
    run(host, tape(4 * kBars), row);
    const auto long_count = host.heap_slots();
    ROW_CHECK(row, host.has_model());
    // Start with four keys. Each bar removes one BEFORE putting a new one,
    // so peak simultaneous live slots is four, and reusing its retired slot
    // keeps the heap table at four through 32 and 128 insert/remove cycles.
    const std::size_t configured_slots = host.live_keys.size();
    ROW_CHECK(row, short_count == configured_slots && long_count == configured_slots
              && short_count == long_count);
    if (!host.values.is_na()) host.values.clear();
    ROW_CHECK(row, host.heap_slots() == 0);
}

struct Row {
    const char* id;
    void (*body)(const char*);
};

void input_cursor_row(const char* row) { scheduler_cursor(row, false); }
void consumed_cursor_row(const char* row) { scheduler_cursor(row, true); }

const Row rows[] = {
    {"firstcallbound_t_object", first_call_object},
    {"pinestrategyhost_aux_security_input_tf", auxiliary_timeframe},
    {"pinescheduler_input_script_cursor", input_cursor_row},
    {"pinescheduler_consumed_script_cursor", consumed_cursor_row},
    {"nativeexecutionconsumer_subscriptions", subscriptions},
    {"nativeexecutionconsumer_barviewbinding_descriptors", bar_view_descriptors},
    {"nativedaypartition_stamps", day_partition},
    {"timeframeaggregator_native_late_opens", aggregator_late_opens},
    {"backtestengine_trace_name_index", trace_index},
    {"backtestengine_native_security_feeds", native_security_feeds},
    {"backtestengine_inputs", inputs},
    {"backtestengine_syminfo_metadata", metadata},
    {"backtestengine_account_currency_fx_rates", fx_rates},
    {"pinemap_storage_heap_values", handle_map_slots},
};
} // namespace

int main() {
    for (const auto& row : rows) {
        try {
            row.body(row.id);
        } catch (const std::exception& error) {
            ++checks;
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s:%d: unexpected exception: %s\n",
                         row.id, __FILE__, __LINE__, error.what());
        } catch (...) {
            ++checks;
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s:%d: unexpected non-standard exception\n",
                         row.id, __FILE__, __LINE__);
        }
    }
    std::printf("memory proof fixed_population: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
