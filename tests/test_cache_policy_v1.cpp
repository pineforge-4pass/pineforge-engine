// Fail-before: compile this unit against the frozen header closure first.
// cache_policy.hpp and the additive C setter are absent there. The runtime
// assertions below also detect ignored sizing, retained caller pointers,
// partial replacements and mid-run changes. No Release-only bare asserts.
#include <pineforge/pineforge.h>
#include <pineforge/cache_policy.hpp>
#include <pineforge/detail/feed_projection.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/run_failure.hpp>
#include <pineforge/series.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "cache_policy_module_fixture.hpp"
#include "../src/cache_policy_validation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
using namespace pineforge;
int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

constexpr std::array<CacheCapacity CacheDefaults::*, 19> members{{
    &CacheDefaults::ledger_rows, &CacheDefaults::trade_pages,
    &CacheDefaults::dependency_pages, &CacheDefaults::exit_pages,
    &CacheDefaults::book_pages, &CacheDefaults::retained_sequence_pages,
    &CacheDefaults::cohort_id_pages, &CacheDefaults::exit_rank_pages,
    &CacheDefaults::erased_minima_pages, &CacheDefaults::consumed_cycle_pages,
    &CacheDefaults::position_tail_pages, &CacheDefaults::successor_root_pages,
    &CacheDefaults::receipt_tail_pages, &CacheDefaults::chain_segment_pages,
    &CacheDefaults::equity_curve_pages, &CacheDefaults::ta_history_pages,
    &CacheDefaults::feed_calendar_pages, &CacheDefaults::recorded_series_pages,
    &CacheDefaults::projection_exit_pages,
}};
constexpr std::array<std::uint32_t, 19> kinds{{
    PF_CACHE_KIND_LEDGER_CACHE, PF_CACHE_KIND_TRADE_PAGES,
    PF_CACHE_KIND_DEPENDENCY_PAGES, PF_CACHE_KIND_EXIT_PAGES,
    PF_CACHE_KIND_BOOK_PAGES, PF_CACHE_KIND_RETAINED_SEQUENCE_PAGES,
    PF_CACHE_KIND_COHORT_ID_PAGES, PF_CACHE_KIND_EXIT_RANK_PAGES,
    PF_CACHE_KIND_ERASED_MINIMA_PAGES, PF_CACHE_KIND_CONSUMED_CYCLE_PAGES,
    PF_CACHE_KIND_POSITION_TAIL_PAGES, PF_CACHE_KIND_SUCCESSOR_ROOT_PAGES,
    PF_CACHE_KIND_RECEIPT_TAIL_PAGES, PF_CACHE_KIND_CHAIN_SEGMENT_PAGES,
    PF_CACHE_KIND_EQUITY_CURVE_PAGES, PF_CACHE_KIND_TA_HISTORY_PAGES,
    PF_CACHE_KIND_FEED_CALENDAR_PAGES, PF_CACHE_KIND_RECORDED_SERIES_PAGES,
    PF_CACHE_KIND_PROJECTION_EXIT_PAGES,
}};

bool same_policy(const CacheDefaults& a, const CacheDefaults& b) {
    for (const auto member : members)
        if ((a.*member).capacity_bytes != (b.*member).capacity_bytes) return false;
    return true;
}

pf_strategy_t handle(BacktestEngine& engine) { return static_cast<pf_strategy_t>(&engine); }

pf_cache_policy_v1 descriptor(const pf_cache_policy_entry_v1* entries, std::uint64_t count) {
    return {sizeof(pf_cache_policy_v1), 1, count, entries, {0, 0}};
}

void rejected(BacktestEngine& engine, const pf_cache_policy_v1& policy, const char* reason) {
    const CacheDefaults before = engine.cache_policy_config();
    CHECK(strategy_set_cache_policy_v1(handle(engine), &policy) == -1);
    CHECK(std::string(strategy_get_last_error_code(handle(engine))) == "setting_rejected");
    CHECK(std::string(strategy_get_last_error_args(handle(engine)))
        == std::string("{\"input\":\"cache_policy\",\"reason\":\"") + reason + "\"}");
    CHECK(same_policy(before, engine.cache_policy_config()));
}

class Probe final : public source::PineStrategyHost {
public:
    std::size_t seen_prepare = 0, seen_bar = 0;
    bool refuse_in_prepare = false, refuse_in_bar = false;
    int prepare_refusal = 0, bar_refusal = 0;
    std::string prepare_code, bar_code;
    std::size_t historical_reads = 0;

    Probe() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
    }
    void prepare_script_run(const Bar*, int, bool) override {
        seen_prepare = cache_policy_for_run().ledger_rows.capacity_bytes;
        historical_reads = 0;
        if (refuse_in_prepare) {
            prepare_refusal = strategy_set_cache_policy_v1(handle(*this), nullptr);
            prepare_code = strategy_get_last_error_code(handle(*this));
        }
    }
    void on_source_bar(const Bar& bar) override {
        seen_bar = cache_policy_for_run().ledger_rows.capacity_bytes;
        if (refuse_in_bar && bar_index_ == 1) {
            bar_refusal = strategy_set_cache_policy_v1(handle(*this), nullptr);
            bar_code = strategy_get_last_error_code(handle(*this));
        }
        // Reversals exercise archived closed rows, command pages and id tables.
        // Read both ends after the fixed descriptor tail has been exceeded.
        if (bar_index_ % 2 == 0)
            strategy_entry("long-entry", true);
        else
            strategy_entry("short-entry", false);
        trace("close", bar.close);
        if (trades_.size() > TradeLedger::kResidentRows) {
            const auto old = trades_.read(0);
            const auto recent = trades_.read(trades_.size() - 1);
            trace("historical-pnl", old.pnl + recent.pnl);
            ++historical_reads;
        }
    }
    std::size_t ledger_rows() const { return trades_.cache_rows(); }
    std::size_t ledger_cached() const { return trades_.cache_size(); }
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (int i = 0; i < 384; ++i) {
        const double price = 100.0 + (i % 23) * 0.25;
        bars.push_back({price, price + 1.0, price - 1.0, price + 0.125,
                        3.0 + i % 5, INT64_C(1704067200000) + i * INT64_C(60000)});
    }
    return bars;
}

void descriptor_validation() {
    CHECK(strategy_set_cache_policy_v1(nullptr, nullptr) == -1);
    bool geometry_rejected = false;
    try {
        (void)CacheDefaults::lines(4096, std::numeric_limits<std::size_t>::max());
    } catch (const std::invalid_argument& error) {
        geometry_rejected = true;
        CHECK(classify_run_failure(error).code == RunFailureCode::setting_rejected);
    }
    CHECK(geometry_rejected);
    Probe engine;
    const CacheDefaults defaults;
    const auto line = TradeLedger::cache_capacity_bytes(1);
    pf_cache_policy_entry_v1 entry{PF_CACHE_KIND_LEDGER_CACHE, 0, line};
    auto policy = descriptor(&entry, 1);
    CHECK(strategy_set_cache_policy_v1(handle(engine), &policy) == 0);
    CHECK(engine.cache_policy_config().ledger_rows.capacity_bytes == line);

    auto bad = policy;
    bad.struct_size = 0;
    rejected(engine, bad, "unparseable_value");
    bad.struct_size = sizeof(policy) - 1;
    rejected(engine, bad, "unparseable_value");
    bad.struct_size = sizeof(policy) + 8;
    rejected(engine, bad, "unparseable_value");
    bad = policy;
    bad.version = 0;
    rejected(engine, bad, "invalid_enum_option");
    bad.version = 2;
    rejected(engine, bad, "invalid_enum_option");
    for (std::size_t i = 0; i < 2; ++i) {
        for (const auto reserved : {UINT32_C(1), UINT32_MAX}) {
            bad = policy;
            bad.reserved[i] = reserved;
            rejected(engine, bad, "unparseable_value");
            bad = descriptor(nullptr, 0);
            bad.reserved[i] = reserved;
            rejected(engine, bad, "unparseable_value");
        }
    }
    for (const auto reserved : {UINT32_C(1), UINT32_MAX}) {
        const pf_cache_policy_entry_v1 nonzero_reserved{
            PF_CACHE_KIND_LEDGER_CACHE, reserved, line};
        bad = descriptor(&nonzero_reserved, 1);
        rejected(engine, bad, "unparseable_value");
    }
    bad = descriptor(nullptr, 1);
    rejected(engine, bad, "unparseable_value");
    bad = descriptor(nullptr, std::numeric_limits<std::uint64_t>::max());
    rejected(engine, bad, "value_above_maximum");

    for (const auto kind : {UINT32_C(0), PF_CACHE_KIND_PROJECTION_EXIT_PAGES + 1u, UINT32_MAX}) {
        const pf_cache_policy_entry_v1 unknown{kind, 0, 0};
        bad = descriptor(&unknown, 1);
        rejected(engine, bad, "unknown_key");
    }
    const pf_cache_policy_entry_v1 duplicates[] = {
        {PF_CACHE_KIND_LEDGER_CACHE, 0, 0}, {PF_CACHE_KIND_LEDGER_CACHE, 0, line * 2}};
    bad = descriptor(duplicates, 2);
    rejected(engine, bad, "ambiguous_key");
    const pf_cache_policy_entry_v1 partial[] = {
        {PF_CACHE_KIND_LEDGER_CACHE, 0, line * 17}, {UINT32_MAX, 0, 0}};
    bad = descriptor(partial, 2);
    rejected(engine, bad, "unknown_key");
    const pf_cache_policy_entry_v1 partial_reserved[] = {
        {PF_CACHE_KIND_LEDGER_CACHE, 0, line * 17},
        {PF_CACHE_KIND_TRADE_PAGES, 1, 0}};
    bad = descriptor(partial_reserved, 2);
    rejected(engine, bad, "unparseable_value");

    // Page kinds have no numeric maximum beyond representability. The ledger
    // retains its existing row setting range, covered separately below.
    entry.kind = PF_CACHE_KIND_TRADE_PAGES;
    for (const auto capacity : {std::size_t(0), std::size_t(1),
                                std::numeric_limits<std::size_t>::max()}) {
        entry.capacity_bytes = capacity;
        CHECK(strategy_set_cache_policy_v1(handle(engine), &policy) == 0);
        CHECK(engine.cache_policy_config().trade_pages.capacity_bytes == capacity);
    }
    if (std::numeric_limits<std::size_t>::max() < std::numeric_limits<std::uint64_t>::max()) {
        entry.capacity_bytes = std::numeric_limits<std::uint64_t>::max();
        rejected(engine, policy, "value_above_maximum");
    }
    const auto empty = descriptor(nullptr, 0);
    CHECK(strategy_set_cache_policy_v1(handle(engine), &empty) == 0);
    CHECK(same_policy(engine.cache_policy_config(), defaults));
    CHECK(strategy_set_cache_policy_v1(handle(engine), nullptr) == 0);
    CHECK(same_policy(engine.cache_policy_config(), defaults));

    // Every kind replaces its intended field; omitted kinds reset to defaults
    // even if the immediately preceding successful call overrode them.
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        const auto capacity = i == 0 ? line + 101 : 101 + i;
        const pf_cache_policy_entry_v1 one{kinds[i], 0, capacity};
        const auto single = descriptor(&one, 1);
        CHECK(strategy_set_cache_policy_v1(handle(engine), &single) == 0);
        for (std::size_t j = 0; j < members.size(); ++j) {
            const auto expected = j == i ? capacity : (defaults.*members[j]).capacity_bytes;
            CHECK((engine.cache_policy_config().*members[j]).capacity_bytes == expected);
        }
    }
}

void ledger_byte_validation() {
    Probe engine;
    const auto entry_bytes = TradeLedger::cache_capacity_bytes(1);
    const auto maximum_bytes = TradeLedger::cache_capacity_bytes(TradeLedger::kMaxCacheRows);
    const auto excessive_bytes = TradeLedger::cache_capacity_bytes(TradeLedger::kMaxCacheRows + 1);
    pf_cache_policy_entry_v1 entry{PF_CACHE_KIND_LEDGER_CACHE, 0, entry_bytes};
    const auto policy = descriptor(&entry, 1);
    CHECK(strategy_set_cache_policy_v1(handle(engine), &policy) == 0);
    for (const auto capacity : {std::size_t(1), TradeLedger::kLedgerEntryBytes,
                                entry_bytes - 1}) {
        entry.capacity_bytes = capacity;
        rejected(engine, policy, "value_below_minimum");
    }
    for (const auto capacity : {excessive_bytes,
                                std::numeric_limits<std::size_t>::max()}) {
        entry.capacity_bytes = capacity;
        rejected(engine, policy, "value_above_maximum");
    }
    // A valid prefix cannot publish if the following ledger entry is invalid.
    for (const auto capacity : {entry_bytes - 1, excessive_bytes}) {
        const pf_cache_policy_entry_v1 entries[] = {
            {PF_CACHE_KIND_TRADE_PAGES, 0, 17},
            {PF_CACHE_KIND_LEDGER_CACHE, 0, capacity}};
        const auto partial = descriptor(entries, 2);
        rejected(engine, partial,
            capacity < entry_bytes ? "value_below_minimum" : "value_above_maximum");
    }
    for (const auto capacity : {std::size_t(0), entry_bytes,
                                TradeLedger::cache_capacity_bytes(2) - 1,
                                maximum_bytes, excessive_bytes - 1}) {
        entry.capacity_bytes = capacity;
        CHECK(strategy_set_cache_policy_v1(handle(engine), &policy) == 0);
        CHECK(engine.cache_policy_config().ledger_rows.capacity_bytes == capacity);
    }
    // The C++ host setter must enforce the same range before replacing config.
    for (const auto capacity : {entry_bytes - 1, excessive_bytes}) {
        const auto before = engine.cache_policy_config();
        auto invalid = before;
        invalid.ledger_rows.capacity_bytes = capacity;
        bool refused = false;
        try { engine.set_cache_policy(invalid); }
        catch (const std::invalid_argument& error) {
            refused = true;
            const auto failure = classify_run_failure(error);
            CHECK(failure.code == RunFailureCode::setting_rejected);
            CHECK(failure.args && *failure.args
                == std::string("{\"input\":\"cache_policy\",\"reason\":\"")
                    + (capacity < entry_bytes ? "value_below_minimum" : "value_above_maximum")
                    + "\"}");
        }
        CHECK(refused && same_policy(before, engine.cache_policy_config()));
    }
}

void retired_kind_validation() {
    const CacheDefaults defaults;
    // Simulate a later release with no active stores. Every listed kind must
    // still be accepted, including when the entry count exceeds the map size.
    const std::array<detail::CachePolicyField, 0> retired{};
    std::array<pf_cache_policy_entry_v1, kinds.size()> entries{};
    for (std::size_t i = 0; i < kinds.size(); ++i)
        entries[i] = {kinds[i], 0, 17 + i};
    auto policy = descriptor(entries.data(), entries.size());
    CHECK(same_policy(detail::copy_cache_policy_v1(&policy, retired), defaults));
    const std::array<detail::CachePolicyField, 1> one_active{{
        {PF_CACHE_KIND_TRADE_PAGES, &CacheDefaults::trade_pages}}};
    auto expected = defaults;
    expected.trade_pages.capacity_bytes = entries[1].capacity_bytes;
    CHECK(same_policy(detail::copy_cache_policy_v1(&policy, one_active), expected));
    // Explicit null members have the same no-op semantics as absent members.
    const std::array<detail::CachePolicyField, 1> null_member{{
        {PF_CACHE_KIND_LEDGER_CACHE, nullptr}}};
    const auto single = descriptor(entries.data(), 1);
    CHECK(same_policy(detail::copy_cache_policy_v1(&single, null_member), defaults));

    auto refuses = [&](const pf_cache_policy_v1& bad, const char* reason) {
        bool refused = false;
        try { (void)detail::copy_cache_policy_v1(&bad, retired); }
        catch (const std::invalid_argument& error) {
            refused = true;
            const auto failure = classify_run_failure(error);
            CHECK(failure.code == RunFailureCode::setting_rejected);
            CHECK(failure.args && *failure.args
                == std::string("{\"input\":\"cache_policy\",\"reason\":\"") + reason + "\"}");
        }
        CHECK(refused);
    };
    entries[1].kind = entries[0].kind;
    refuses(policy, "ambiguous_key");
    entries[1].kind = kinds[1];
    entries.back().kind = PF_CACHE_KIND_PROJECTION_EXIT_PAGES + 1u;
    refuses(policy, "unknown_key");
    entries.back().kind = kinds.back();
    entries.back().reserved = 1;
    refuses(policy, "unparseable_value");
}

void copied_ledger_policy() {
    const auto line = TradeLedger::cache_capacity_bytes(1);
    for (const auto rows : {std::size_t(0), TradeLedger::kMaxCacheRows + 1}) {
        TradeLedger original(CacheCapacity{TradeLedger::cache_capacity_bytes(rows)});
        Trade row{};
        row.entry_id = "copied-policy";
        row.exit_comment = "retained-row";
        row.pnl = 12.5;
        original.push_back(row);
        TradeLedger copy(original);
        TradeLedger assigned;
        assigned = original;
        TradeLedger moved(std::move(copy));
        CHECK(original.cache_rows() == rows && moved.cache_rows() == rows);
        CHECK(assigned.cache_rows() == rows);
        for (const auto* ledger : {&original, &assigned, &moved}) {
            const auto value = ledger->read(0);
            CHECK(value.entry_id == row.entry_id && value.exit_comment == row.exit_comment);
            CHECK(value.pnl == row.pnl && ledger->size() == 1);
        }
        bool frozen = false;
        try { original.set_cache_capacity(CacheCapacity{line}); }
        catch (const std::invalid_argument& error) {
            frozen = true;
            CHECK(classify_run_failure(error).code == RunFailureCode::setting_rejected);
        }
        CHECK(frozen && original.cache_rows() == rows);
    }
}

void copy_and_next_run(const std::vector<Bar>& bars) {
    Probe engine;
    const CacheDefaults defaults;
    const auto line = TradeLedger::cache_capacity_bytes(1);
    const auto next_bytes = TradeLedger::cache_capacity_bytes(128);
    {
        pf_cache_policy_entry_v1 entries[] = {{PF_CACHE_KIND_LEDGER_CACHE, 0, line}};
        auto policy = descriptor(entries, 1);
        CHECK(strategy_set_cache_policy_v1(handle(engine), &policy) == 0);
        entries[0] = {UINT32_MAX, 0, 0};
        policy.version = 0;
        policy.entries = nullptr;
    } // Neither descriptor nor entries is alive when the run is admitted.
    CHECK(engine.cache_policy_config().ledger_rows.capacity_bytes == line);
    CHECK(engine.cache_policy_for_run().ledger_rows.capacity_bytes
        == defaults.ledger_rows.capacity_bytes);
    engine.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(engine.last_run_status() == 0 && engine.last_error().empty());
    CHECK(engine.seen_prepare == line && engine.seen_bar == line);
    CHECK(engine.ledger_rows() == 1 && engine.ledger_cached() <= 1);
    CHECK(engine.trade_count() > static_cast<int>(TradeLedger::kResidentRows));
    CHECK(engine.historical_reads > 0);

    pf_cache_policy_entry_v1 next{PF_CACHE_KIND_LEDGER_CACHE, 0, next_bytes};
    const auto next_policy = descriptor(&next, 1);
    CHECK(strategy_set_cache_policy_v1(handle(engine), &next_policy) == 0);
    CHECK(engine.cache_policy_config().ledger_rows.capacity_bytes == next_bytes);
    CHECK(engine.cache_policy_for_run().ledger_rows.capacity_bytes == line);
    CHECK(engine.ledger_rows() == 1); // The preceding generation was not resized.
    engine.run(nullptr, -1);
    CHECK(!engine.last_error().empty());
    CHECK(engine.cache_policy_for_run().ledger_rows.capacity_bytes == line);
    CHECK(engine.cache_policy_config().ledger_rows.capacity_bytes == next_bytes);
    CHECK(engine.ledger_rows() == 1);
    engine.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(engine.last_run_status() == 0 && engine.last_error().empty());
    CHECK(engine.seen_prepare == next_bytes && engine.seen_bar == next_bytes);
    CHECK(engine.ledger_rows() == 128 && engine.ledger_cached() > 1);
    CHECK(strategy_set_cache_policy_v1(handle(engine), nullptr) == 0);
    CHECK(engine.cache_policy_for_run().ledger_rows.capacity_bytes == next_bytes);
    engine.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(engine.last_run_status() == 0 && engine.last_error().empty());
    CHECK(engine.ledger_rows() == TradeLedger::kDefaultCacheRows);

    next.capacity_bytes = TradeLedger::cache_capacity_bytes(TradeLedger::kMaxCacheRows + 1) - 1;
    CHECK(strategy_set_cache_policy_v1(handle(engine), &next_policy) == 0);
    engine.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(engine.last_run_status() == 0 && engine.last_error().empty());
    CHECK(engine.ledger_rows() == TradeLedger::kMaxCacheRows);
    CHECK(engine.historical_reads > 0);
}

void run_freeze(const std::vector<Bar>& bars) {
    Probe engine;
    CacheDefaults policy;
    policy.ledger_rows = CacheCapacity{TradeLedger::cache_capacity_bytes(2)};
    engine.set_cache_policy(policy);
    engine.refuse_in_prepare = engine.refuse_in_bar = true;
    engine.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(engine.prepare_refusal == -1 && engine.prepare_code == "setting_rejected");
    CHECK(engine.bar_refusal == -1 && engine.bar_code == "setting_rejected");
    CHECK(engine.last_run_status() == 0);
    CHECK(engine.seen_prepare == policy.ledger_rows.capacity_bytes);
    CHECK(engine.seen_bar == policy.ledger_rows.capacity_bytes);
    CHECK(same_policy(engine.cache_policy_config(), policy));
    CHECK(same_policy(engine.cache_policy_for_run(), policy));
    CHECK(engine.ledger_rows() == 2);

    Probe stream;
    stream.set_cache_policy(policy);
    pf_bar_t warmup{100.0, 101.0, 99.0, 100.0, 1.0, INT64_C(1704067200000)};
    CHECK(strategy_stream_begin(handle(stream), &warmup, 1, "1", "1") == 0);
    CHECK(strategy_set_cache_policy_v1(handle(stream), nullptr) == -1);
    CHECK(std::string(strategy_get_last_error_code(handle(stream))) == "setting_rejected");
    CHECK(same_policy(stream.cache_policy_config(), policy));
    CHECK(same_policy(stream.cache_policy_for_run(), policy));
    CHECK(strategy_stream_end(handle(stream), 0) == 0);
    CHECK(strategy_set_cache_policy_v1(handle(stream), nullptr) == 0);
    CHECK(same_policy(stream.cache_policy_config(), CacheDefaults{}));
}

struct TemporaryDirectory {
    char name[64] = "/tmp/pineforge-cache-policy-XXXXXX";
    TemporaryDirectory() {
        if (!::mkdtemp(name)) throw std::runtime_error("cache policy test directory");
    }
    ~TemporaryDirectory() {
        for (const char* file : {"bypass", "subline", "tiny", "large"})
            ::unlink((std::string(name) + "/" + file).c_str());
        ::rmdir(name);
    }
};

struct Result {
    std::string bytes;
    std::uint64_t continuation = 0;
    std::uint64_t broker = 0;
    std::size_t cached_rows = 0;
};

Result run_sink(const std::vector<Bar>& bars, const CacheDefaults& settings, const std::string& path) {
    Probe engine;
    std::array<pf_cache_policy_entry_v1, kinds.size()> entries{};
    for (std::size_t i = 0; i < kinds.size(); ++i)
        entries[i] = {kinds[i], 0, (settings.*members[i]).capacity_bytes};
    const auto policy = descriptor(entries.data(), entries.size());
    CHECK(strategy_set_cache_policy_v1(handle(engine), &policy) == 0);
    const auto bytes = settings.ledger_rows.capacity_bytes;
    engine.set_trace_enabled(true);
    engine.set_broker_state_hash_recording(true);
    const pf_report_sink_v1 sink{sizeof(sink), 1, path.c_str(), "cache-policy", nullptr};
    CHECK(engine.set_report_sink_v1(&sink) == 0);
    engine.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(engine.last_run_status() == 0 && engine.last_error().empty());
    CHECK(engine.historical_reads > 0);
    CHECK(engine.seen_prepare == bytes && engine.seen_bar == bytes);
    ReportC report{};
    engine.fill_report(&report);
    BacktestEngine::free_report(&report);
    pf_report_sink_receipt_v1 receipt{};
    receipt.struct_size = sizeof(receipt);
    receipt.version = 1;
    CHECK(engine.report_sink_receipt_v1(&receipt) == 0 && receipt.complete == 1);
    std::ifstream file(path, std::ios::binary);
    CHECK(bool(file));
    Result result;
    result.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    result.continuation = engine.stream_state_hash();
    result.broker = engine.broker_state_hash();
    result.cached_rows = engine.ledger_cached();
    return result;
}

void byte_identity(const std::vector<Bar>& bars) {
    TemporaryDirectory directory;
    const std::string base = std::string(directory.name) + "/";
    const auto line = TradeLedger::cache_capacity_bytes(1);
    const auto large = run_sink(bars, CacheDefaults{}, base + "large");
    CHECK(large.bytes.find("[\"T\",") != std::string::npos);
    CHECK(large.bytes.find("[\"E\",") != std::string::npos);
    CHECK(large.bytes.find("[\"X\",") != std::string::npos);
    CHECK(large.bytes.find("[\"H\",") != std::string::npos);
    CHECK(large.cached_rows > 1);
    CacheDefaults bypass, subline, tiny;
    for (const auto member : members) {
        (bypass.*member).capacity_bytes = 0;
        (subline.*member).capacity_bytes = 1;
    }
    // The ledger rejects nonzero sub-entry bytes; zero is its explicit bypass.
    // The subline run still supplies one byte to every other cache kind.
    subline.ledger_rows.capacity_bytes = 0;
    const auto platform_page = ::sysconf(_SC_PAGESIZE);
    if (platform_page <= 0) throw std::runtime_error("cache policy platform page size");
    const std::array<std::size_t, 19> one_line{{
        line, detail::ReadThroughCache::line_bytes(512),
        detail::ReadThroughCache::line_bytes(512),
        detail::ReadThroughCache::line_bytes(static_cast<std::size_t>(platform_page)),
        detail::ReadThroughCache::line_bytes(512), detail::ReadThroughCache::line_bytes(512),
        detail::ReadThroughCache::line_bytes(4096), detail::ReadThroughCache::line_bytes(4096),
        detail::ReadThroughCache::line_bytes(4096), detail::ReadThroughCache::line_bytes(4096),
        detail::ReadThroughCache::line_bytes(64 * 1024), detail::ReadThroughCache::line_bytes(16),
        detail::ReadThroughCache::line_bytes(32), detail::ReadThroughCache::line_bytes(24),
        detail::ReadThroughCache::line_bytes(24), detail::ReadThroughCache::line_bytes(1024 * sizeof(double)),
        detail::FeedProjection<std::int64_t>::page_bytes(),
        detail::FeedProjection<source::RecordedSeriesRow>::page_bytes(),
        detail::ReadThroughCache::line_bytes(512),
    }};
    for (std::size_t i = 0; i < members.size(); ++i) (tiny.*members[i]).capacity_bytes = one_line[i];
    for (const auto& setting : std::array<std::pair<const char*, CacheDefaults>, 3>{{
             {"bypass", bypass}, {"subline", subline}, {"tiny", tiny}}}) {
        const auto small = run_sink(bars, setting.second, base + setting.first);
        CHECK(small.bytes == large.bytes); // Complete sink, including its trailer.
        CHECK(small.continuation == large.continuation);
        CHECK(small.broker == large.broker);
        CHECK(small.cached_rows <= (setting.second.ledger_rows.capacity_bytes == line ? 1U : 0U));
    }
}

void module_constructors() {
    void* module = ::dlopen(PF_CACHE_POLICY_MODULE_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!module) throw std::runtime_error(::dlerror());
    struct CloseModule {
        void* module;
        ~CloseModule() { ::dlclose(module); }
    } close_module{module};
    using Create = pf_strategy_t (*)();
    using Free = void (*)(pf_strategy_t);
    using Set = int (*)(pf_strategy_t, const pf_cache_policy_v1*);
    using Capture = int (*)(pf_strategy_t, const pf_bar_t*, int, double*, std::size_t,
                            CachePolicyModuleObservation*);
    const auto create = reinterpret_cast<Create>(::dlsym(module, "cache_policy_module_create"));
    const auto release = reinterpret_cast<Free>(::dlsym(module, "cache_policy_module_free"));
    const auto set = reinterpret_cast<Set>(::dlsym(module, "strategy_set_cache_policy_v1"));
    const auto capture = reinterpret_cast<Capture>(::dlsym(module, "cache_policy_module_capture"));
    if (!create || !release || !set || !capture)
        throw std::runtime_error("cache policy module export is absent");

    std::vector<pf_bar_t> bars;
    const auto count = kCachePolicyModuleValues * 2 + 7;
    for (std::size_t i = 0; i < count; ++i) {
        const double price = 100.0 + (i % 23) * 0.25;
        bars.push_back({price, price + 1.0, price - 1.0, price + 0.125, 1.0,
            INT64_C(1704067200000) + static_cast<std::int64_t>(i) * INT64_C(60000)});
    }
    const auto page_bytes = detail::WindowValues<double>::kChunk * sizeof(double);
    const auto line = detail::ReadThroughCache::line_bytes(page_bytes);
    std::array<double, kCachePolicyModuleValues> reference{};
    bool have_reference = false;
    for (const auto capacity : {line * 3, std::size_t(0), line - 1, line,
                               line * 2 - 1, line * 2, line * 3}) {
        pf_strategy_t strategy = create();
        if (!strategy) throw std::runtime_error("cache policy module create failed");
        struct Release {
            pf_strategy_t strategy;
            Free release;
            ~Release() { release(strategy); }
        } owner{strategy, release};
        pf_cache_policy_entry_v1 entry{PF_CACHE_KIND_TA_HISTORY_PAGES, 0, capacity};
        auto policy = descriptor(&entry, 1);
        CHECK(set(strategy, &policy) == 0);
        entry = {UINT32_MAX, 0, 0};
        policy.entries = nullptr;
        std::array<double, kCachePolicyModuleValues> output{};
        CachePolicyModuleObservation observation;
        CHECK(capture(strategy, bars.data(), static_cast<int>(bars.size()),
                      output.data(), output.size(), &observation) == 0);
        CHECK(observation.pages == capacity / line);
        CHECK(observation.read_cache_payload_bytes
              + observation.read_cache_metadata_bytes <= capacity);
        CHECK(observation.resident_metadata_bytes > observation.read_cache_metadata_bytes);
        if (capacity < line * 3) {
            CHECK(observation.backing_reads > 0 && observation.backing_read_bytes > 0);
            // Owner metadata includes inline control and the backing wrapper,
            // even at zero capacity. Bound the real shared payload AND tags.
            const auto pages = capacity / line;
            CHECK(observation.read_cache_payload_bytes == pages * page_bytes);
            CHECK(observation.read_cache_metadata_bytes == pages * (line - page_bytes));
            CHECK(observation.resident_payload_bytes == observation.read_cache_payload_bytes);
        } else {
            CHECK(observation.read_cache_payload_bytes == 0
                  && observation.read_cache_metadata_bytes == 0);
            CHECK(observation.resident_payload_bytes > 0);
            CHECK(observation.backing_reads == 0 && observation.backing_read_bytes == 0);
        }
        for (std::size_t i = 0; i < output.size(); ++i)
            CHECK(output[i] == bars[bars.size() - 1 - i].close);
        if (have_reference)
            CHECK(std::memcmp(output.data(), reference.data(), sizeof(reference)) == 0);
        else {
            reference = output;
            have_reference = true;
        }
        CHECK(window_cache_capacity().pages == WindowCacheCapacity::kKeepResident);
    }
}
} // namespace

int main() {
    try {
        const auto bars = feed();
        descriptor_validation();
        ledger_byte_validation();
        retired_kind_validation();
        copied_ledger_policy();
        copy_and_next_run(bars);
        run_freeze(bars);
        byte_identity(bars);
        module_constructors();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL cache policy exception: %s\n", error.what());
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
