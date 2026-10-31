#pragma once

#include "acceptance_process_fixture.hpp"
#include "acceptance_sink_fixture.hpp"
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace cache_sweep {
using acceptance_process::require;
namespace pf = pineforge;

// Explicit fields only: no padding, addresses, timing or normalized doubles.
template<class T>
void scalar(std::string& bytes, T value) {
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
    bytes.append(reinterpret_cast<const char*>(&value), sizeof value);
}
inline void text(std::string& bytes, const std::string& value) {
    scalar(bytes, static_cast<std::uint64_t>(value.size()));
    bytes.append(value);
}
inline void trade(std::string& bytes, const pf::Trade& t) {
    scalar(bytes, t.entry_time); scalar(bytes, t.exit_time);
    scalar(bytes, t.entry_price); scalar(bytes, t.exit_price);
    scalar(bytes, t.qty); scalar(bytes, t.pnl); scalar(bytes, t.pnl_pct);
    scalar(bytes, t.is_long); scalar(bytes, t.entry_bar_index); scalar(bytes, t.exit_bar_index);
    text(bytes, t.entry_id); text(bytes, t.entry_comment);
    text(bytes, t.exit_comment); text(bytes, t.exit_id);
    scalar(bytes, t.exit_from_bracket); scalar(bytes, t.max_runup); scalar(bytes, t.max_drawdown);
    scalar(bytes, t.commission); scalar(bytes, t.entry_incarnation);
    scalar(bytes, t.open_at_end); scalar(bytes, t.close_cause);
}

struct Output {
    std::string records, continuation_hashes, trades, fills, report_bytes;
};
struct Telemetry {
    std::uint64_t configured_payload = 0, configured_metadata = 0;
    std::uint64_t payload = 0, metadata = 0, peak_resident = 0, workspace = 0;
    std::uint64_t fixed_control = 0, fixed_workspace = 0;
    std::uint64_t hits = 0, misses = 0, evictions = 0;
    // Count at the inner primitive, independently of cache statistics and
    // logical charged-I/O observers. Include failed physical attempts.
    std::uint64_t inner_reads = 0, inner_bytes = 0;
    std::uint64_t cold_reads = 0, evicted_reread_reads = 0;
    std::uint64_t fault_reads = 0;
};
struct Result { Output output; Telemetry cache; };
struct Setting {
    std::size_t units;
    bool defaults;
    const char* name;
};

inline bool cold_read_proven(const Telemetry& stats) {
    return stats.inner_reads > 0 && stats.inner_bytes > 0
        && stats.cold_reads > 0 && stats.cold_reads <= stats.inner_reads;
}

// A real broker replay consumes bytes returned by the selected store.
// The sink retains all history; no report mode changes with cache size.
inline void replay(Output& output) {
    class Host final : public pf::source::PineStrategyHost {
    public:
        Host(const std::string& records, Output& result) : records_(records), result_(result) {
            pf::source::PineStrategyConfig config;
            config.initial_capital = 100000;
            config.default_qty_type = static_cast<int>(pf::QtyType::FIXED);
            config.default_qty_value = 1;
            config.margin_long = config.margin_short = 0;
            configure_pine_strategy(config);
            set_syminfo_timezone("UTC");
            set_syminfo_session("24x7");
            set_syminfo_mintick(.25);
            set_broker_state_hash_recording(true);
        }
        void on_source_bar(const pf::Bar&) override {
            scalar(result_.continuation_hashes, broker_state_hash());
            const auto b = callbacks++;
            if (b >= 128) return;
            if (b % 4 == 0) {
                const auto byte = static_cast<unsigned char>(records_[(b * 7919) % records_.size()]);
                strategy_entry("E", true, std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::quiet_NaN(), 1 + (byte % 3) * .25);
            } else if (b % 4 == 2) strategy_close("E");
        }
        std::size_t callbacks = 0, applied = 0;
    protected:
        void on_native_applied_event(const pf::native_order::ExecutionAppliedEvent& event,
                                    const pf::NativeDecisionContext&) override {
            ++applied;
            const pf::native_order::CommandEvent command = event;
            const auto source = pf::native_order::encode_journal_record(command);
            require(source.size() > 0, "applied fill codec produced an empty record");
            scalar(result_.fills, source.size());
            std::array<std::byte, 512> buffer{};
            for (std::uint64_t offset = 0; offset < source.size();) {
                const auto count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(buffer.size(), source.size() - offset));
                require(source.read(offset, {buffer.data(), count}) == pf::detail::ScratchStatus::ok,
                        "cannot encode complete applied fill");
                result_.fills.append(reinterpret_cast<const char*>(buffer.data()), count);
                offset += count;
            }
        }
    private:
        const std::string& records_;
        Output& result_;
    };
    require(!output.records.empty(), "store workload produced no complete records");
    acceptance_sink::File sink;
    Host host(output.records, output);
    sink.install(host, false);
    std::vector<pf::Bar> bars;
    for (int i = 0; i < 130; ++i)
        bars.push_back({100, 101, 99, 100 + (i % 3 - 1) * .25, 1,
            INT64_C(1736121600000) + static_cast<std::int64_t>(i) * 60000});
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_run_status() == 0 && host.last_error().empty(), "cache replay failed");
    require(host.callbacks == bars.size() && host.applied == 64 && host.trade_count() == 32,
            "cache replay omitted fills or trades");
    acceptance_sink::Report report;
    host.fill_report(&report.value);
    sink.completed(host);
    for (std::size_t i = 0; i < host.closed_trade_count(); ++i)
        trade(output.trades, host.closed_trade(i));
    scalar(output.continuation_hashes, host.broker_state_hash());
    pf_report_sink_receipt_v1 receipt{};
    receipt.struct_size = sizeof receipt;
    receipt.version = 1;
    require(host.report_sink_receipt_v1(&receipt) == 0
        && receipt.count_h == static_cast<std::int64_t>(bars.size()) && receipt.count_t == 32,
        "cache replay omitted complete sink trade/hash rows");
    output.report_bytes = sink.bytes();
}

inline void same_bytes(const char* label, const std::string& a, const std::string& b) {
    if (a != b) {
        std::fprintf(stderr, "cache output drift field=%s bytes=%zu/%zu\n", label, a.size(), b.size());
        throw std::runtime_error("cache size changed complete output bytes");
    }
}
inline void compare(const Output& a, const Output& b) {
    same_bytes("all store records", a.records, b.records);
    same_bytes("continuation hashes", a.continuation_hashes, b.continuation_hashes);
    same_bytes("all trade fields", a.trades, b.trades);
    same_bytes("complete fill codec records", a.fills, b.fills);
    same_bytes("complete sink report", a.report_bytes, b.report_bytes);
}

template<class Runner>
void sweep(const char* kind, std::size_t one_line_units, std::size_t default_units,
           std::size_t large_units, Runner run) {
    const std::array<Setting, 5> settings{{
        {0, false, "bypass"}, {one_line_units, false, "one_line"},
        {2 * one_line_units, false, "two_lines"}, {default_units, true, "default"},
        {large_units, false, "large"}
    }};
    Output baseline;
    for (std::size_t i = 0; i < settings.size(); ++i) {
        auto result = run(settings[i]); // The runner receives no population/density knob.
        auto& s = result.cache;
        require(s.payload <= s.configured_payload && s.metadata <= s.configured_metadata
            && s.peak_resident <= s.configured_payload + s.configured_metadata,
            "cache resident payload/metadata exceeded configured bytes");
        require(cold_read_proven(s), "cold read never reached the inner backing");
        require(s.misses > 0, "cache sweep never observed a miss");
        if (i == 0) require(s.payload == 0 && s.hits == 0, "zero cache secretly retained payload");
        if (i == 1 || i == 2)
            require(s.payload > 0 && s.hits > 0 && s.evictions > 0 && s.evicted_reread_reads > 0,
                    "tiny sweep did not allocate, hit, evict and physically reread an early record");
        replay(result.output);
        std::printf("CACHE_SWEEP kind=%s setting=%s units=%zu "
                    "configured_payload=%llu configured_metadata=%llu payload=%llu metadata=%llu "
                    "peak_resident=%llu workspace=%llu fixed_control=%llu fixed_workspace=%llu "
                    "hits=%llu misses=%llu evictions=%llu "
                    "inner_reads=%llu inner_bytes=%llu cold_reads=%llu evicted_reread_reads=%llu "
                    "fault_reads=%llu\n",
            kind, settings[i].name, settings[i].units,
            static_cast<unsigned long long>(s.configured_payload),
            static_cast<unsigned long long>(s.configured_metadata),
            static_cast<unsigned long long>(s.payload),
            static_cast<unsigned long long>(s.metadata),
            static_cast<unsigned long long>(s.peak_resident),
            static_cast<unsigned long long>(s.workspace),
            static_cast<unsigned long long>(s.fixed_control),
            static_cast<unsigned long long>(s.fixed_workspace),
            static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.misses),
            static_cast<unsigned long long>(s.evictions),
            static_cast<unsigned long long>(s.inner_reads), static_cast<unsigned long long>(s.inner_bytes),
            static_cast<unsigned long long>(s.cold_reads),
            static_cast<unsigned long long>(s.evicted_reread_reads),
            static_cast<unsigned long long>(s.fault_reads));
        if (i == 0) baseline = std::move(result.output);
        else compare(baseline, result.output);
    }
}
} // namespace cache_sweep
