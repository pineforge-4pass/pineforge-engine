// Real source-host retention witness. The fixture owns a production-shaped
// exit-scratch store, drives a nonempty order/fill/close workload at two
// populations, and compares an independently specified report witness.
#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/source/exit_scratch.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/exit_scratch_versions_production.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace pineforge;
namespace sd = pineforge::source::detail;

template<class Tag, auto Member>
struct MemberAccess {
    friend auto member(Tag) { return Member; }
};
struct AdapterMember {
    friend auto member(AdapterMember);
};
template struct MemberAccess<AdapterMember, &source::PineStrategyHost::adapter_>;

source::PineExecutionAdapter& adapter(source::PineStrategyHost& host) {
    return host.*member(AdapterMember{});
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct ExitStoreFixture {
    static constexpr std::size_t page_size = 512;
    std::vector<std::byte> bytes = std::vector<std::byte>(64u * 1024u * 1024u);
    detail::FixedMemoryScratchBacking backing{
        {bytes.data(), bytes.size()}, page_size};
    detail::CommandScratchStore store{
        {2, page_size, 1, 1, sd::exit_scratch_key_order(),
         detail::ScratchOpenMode::create_empty},
        &backing};
    sd::ExitScratchVersionsProduction versions{store};
    sd::ExitScratchContext context{store, {}};

    ExitStoreFixture() {
        require(backing.status() == detail::ScratchStatus::ok
                    && store.status() == detail::ScratchStatus::ok,
                "exit fixture store did not open");
        context.versions = &versions;
    }
};

struct RunIdentity {
    std::size_t bars = 0;
    std::size_t commands = 0;
    std::size_t applied = 0;
    std::size_t closed = 0;
    std::size_t peak_extent = 0;
    std::uint64_t schedule_hash = 1469598103934665603ULL;
    std::string report_bytes;
    std::string failure;
};

template<class T>
void append_bytes(std::string& out, const T& value) {
    const auto* bytes = reinterpret_cast<const char*>(&value);
    out.append(bytes, sizeof(value));
}

void fold_schedule(std::uint64_t& hash, std::uint64_t word) {
    for (unsigned shift = 0; shift != 64; shift += 8) {
        hash ^= static_cast<unsigned char>(word >> shift);
        hash *= 1099511628211ULL;
    }
}

std::vector<Bar> tape(std::size_t count) {
    std::vector<Bar> bars;
    bars.reserve(count);
    constexpr std::int64_t first = 1704067200000LL;
    for (std::size_t index = 0; index < count; ++index) {
        bars.push_back(Bar{100.0, 101.0, 99.0, 100.0, 1.0,
            first + static_cast<std::int64_t>(index) * 60000});
    }
    return bars;
}

class EventHost final : public source::PineStrategyHost {
public:
    explicit EventHost(ExitStoreFixture& fixture) : fixture_(fixture) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = true;
        config.commission_value = 0.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar& bar) override {
        if (!bound_) bind_exit_store();
        ++identity_.bars;
        fold_schedule(identity_.schedule_hash,
                      static_cast<std::uint64_t>(bar_index_));
        fold_schedule(identity_.schedule_hash,
                      static_cast<std::uint64_t>((bar_index_ & 1) == 0 ? 1 : 2));
        identity_.peak_extent = std::max<std::size_t>(
            identity_.peak_extent,
            static_cast<std::size_t>(fixture_.store.stats().backing_bytes));

        if ((bar_index_ & 1) == 0) {
            ++identity_.commands;
            strategy_entry("entry", true, missing(), missing(), 1.0,
                           "entry-" + std::to_string(bar_index_));
        } else {
            ++identity_.commands;
            strategy_close("entry");
        }
        identity_.peak_extent = std::max<std::size_t>(
            identity_.peak_extent,
            static_cast<std::size_t>(fixture_.store.stats().backing_bytes));
    }

    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        ++identity_.applied;
        identity_.closed += event.closed_trade_count;
        identity_.peak_extent = std::max<std::size_t>(
            identity_.peak_extent,
            static_cast<std::size_t>(fixture_.store.stats().backing_bytes));
    }

    RunIdentity finish(std::size_t count) {
        identity_.failure = last_error();
        require(identity_.bars == count, "the source callback count changed");
        require(identity_.commands == count, "the script did not issue one command per bar");
        require(identity_.applied > 0 && identity_.closed > 0,
                "the workload produced no applied fill or close");
        require(fixture_.store.record_count() == 0
                    && !fixture_.store.transaction_active(),
                "exit scope rows survived the run");

        ReportC report{};
        fill_report(&report);
        require(report.trades_len == static_cast<std::int64_t>(count / 2),
                "the independently expected trade count changed");
        require(report.trades != nullptr && report.equity_curve != nullptr,
                "the real report did not contain its identity arrays");
        identity_.report_bytes = report_witness(report, count);
        BacktestEngine::free_report(&report);
        return identity_;
    }

    const RunIdentity& identity() const noexcept { return identity_; }

private:
    static double missing() noexcept {
        return std::numeric_limits<double>::quiet_NaN();
    }

    void bind_exit_store() {
        auto& current = adapter(*this);
        auto& original = sd::exit_scratch(current);
        fixture_.context.run = original.run;
        fixture_.context.observation = original.observation;
        fixture_.context.observe_rows = original.observe_rows;
        fixture_.context.observe_lookup = original.observe_lookup;
        fixture_.context.lots = original.lots;
        fixture_.context.working = original.working;
        current.bind_exit_scratch(fixture_.context);
        bound_ = true;
    }

    static std::string report_witness(const ReportC& report, std::size_t count) {
        std::string bytes;
        append_bytes(bytes, report.total_trades);
        append_bytes(bytes, report.trades_len);
        for (std::size_t index = 0; index < count / 2; ++index) {
            const auto& row = report.trades[index];
            const auto expected_entry = 1704067200000LL
                + static_cast<std::int64_t>(2 * index) * 60000;
            const auto expected_exit = expected_entry + 60000;
            require(row.entry_time == expected_entry && row.exit_time == expected_exit,
                    "trade timestamps differ from the independent tape oracle");
            require(row.entry_price == 100.0 && row.exit_price == 100.0
                        && row.qty == 1.0 && row.pnl == 0.0
                        && row.pnl_pct == 0.0 && row.is_long != 0
                        && row.entry_bar_index == static_cast<int>(2 * index)
                        && row.exit_bar_index == static_cast<int>(2 * index + 1)
                        && row.open_at_end == 0,
                    "trade fields differ from the independent tape oracle");
            append_bytes(bytes, row.entry_time);
            append_bytes(bytes, row.exit_time);
            append_bytes(bytes, row.entry_price);
            append_bytes(bytes, row.exit_price);
            append_bytes(bytes, row.pnl);
            append_bytes(bytes, row.pnl_pct);
            append_bytes(bytes, row.is_long);
            append_bytes(bytes, row.qty);
            append_bytes(bytes, row.entry_bar_index);
            append_bytes(bytes, row.exit_bar_index);
        }
        require(report.equity_curve_len == static_cast<std::int64_t>(count),
                "equity point count differs from the input population");
        append_bytes(bytes, report.equity_curve_len);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& point = report.equity_curve[index];
            require(point.time_ms == 1704067200000LL
                        + static_cast<std::int64_t>(index) * 60000
                        && point.equity == 1000000.0
                        && point.open_profit == 0.0,
                    "equity point differs from the independent tape oracle");
            append_bytes(bytes, point.time_ms);
            append_bytes(bytes, point.equity);
            append_bytes(bytes, point.open_profit);
        }
        return bytes;
    }

    ExitStoreFixture& fixture_;
    RunIdentity identity_{};
    bool bound_ = false;
};

RunIdentity run_population(std::size_t count) {
    require(count > 0 && (count % 2) == 0, "population must be positive and even");
    ExitStoreFixture fixture;
    EventHost host(fixture);
    const auto bars = tape(count);
    host.run(bars.data(), static_cast<int>(bars.size()));
    const auto result = host.finish(count);
    require(result.failure.empty() && host.last_run_status() == 0,
            "the real engine reported a typed failure on the identity workload");
    return result;
}

void compare_identity(const RunIdentity& result, std::size_t count) {
    require(result.bars == count && result.commands == count,
            "identity counters did not cover the full population");
    require(result.closed == count / 2, "closed-trade identity count changed");
    require(!result.report_bytes.empty(), "report byte witness is empty");
    require(result.schedule_hash != 1469598103934665603ULL,
            "per-bar identity hash did not execute");
}

} // namespace

int main() {
    try {
        const auto small = run_population(4000);
        const auto large = run_population(16000);
        compare_identity(small, 4000);
        compare_identity(large, 16000);
        constexpr std::size_t slack = 16 * ExitStoreFixture::page_size;
        require(large.peak_extent <= small.peak_extent + slack,
                "exit scratch extent grows with the event population");
        require(large.report_bytes.size() > small.report_bytes.size(),
                "report bytes did not cover the larger independent population");
        std::printf("event retention: N=4000 peak=%zu, N=16000 peak=%zu PASS\n",
                    small.peak_extent, large.peak_extent);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "event retention: %s\n", error.what());
        return 1;
    }
}
