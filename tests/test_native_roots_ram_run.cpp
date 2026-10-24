// CR-150 root binding through a real sink-OFF native begin. Predicted red on
// d91c96d47d: the RAM twin fails before on_native_run_begin with the foundation's
// "bounded successor-root scratch binding is not installed" engine_invariant.
#include <pineforge/pineforge.h>
#include <pineforge/native_host.hpp>
#include <pineforge/detail/scratch_slab.hpp>
#include <pineforge/run_failure.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t bits(double value) {
    static_assert(sizeof(value) == sizeof(std::uint64_t), "binary64 observations");
    std::uint64_t word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

NativeRunSpec spec() {
    NativeRunSpec value;
    value.identity = {"ram-root-run", 1};
    value.input_tf = value.script_tf = "1";
    value.ticker = "MOCK";
    value.tickerid = "TEST:MOCK";
    value.type = "crypto";
    value.currency = "USD";
    value.timezone = "UTC";
    value.session = "24x7";
    value.initial_capital = 10000.0;
    value.point_value = value.account_fx = 1.0;
    value.price_tick = 0.01;
    value.fee_kind = NativeFeeKind::CashPerExecution;
    value.fee_value = 0.0;
    value.report_policy = NativeReportPolicy::KernelRecorded;
    return value;
}

// 129 alternating successors plus one market replacement = 130 root rows.
// The last four bars allow the market successor and its rootless close to
// execute at subsequent points. All prices are exactly 100 in binary64.
constexpr std::size_t kRootRows = 2 * 64 + 2;
constexpr std::size_t kBars = kRootRows + 4;

std::array<Bar, kBars> bars() {
    std::array<Bar, kBars> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = {100.0, 100.0, 100.0, 100.0, 1.0,
                     1704067200000LL + static_cast<std::int64_t>(i) * 60000};
    return result;
}

class RootsHost final : public NativeStrategyHost {
public:
    std::size_t begin_calls = 0;
    std::size_t bar_calls = 0;
    std::size_t replacements = 0;
    std::shared_ptr<detail::ScratchSlab> observed_slab;
    std::vector<std::uint64_t> callback_continuations;
    std::vector<std::uint64_t> callback_brokers;

    void on_native_run_begin() override {
        ++begin_calls;
        observed_slab = scratch_tables_for_run();
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const auto index = bar_calls++;
        if (index == 0) {
            for (std::size_t slot = 0; slot < tails_.size(); ++slot) {
                const auto accepted = submit(opening(slot, false));
                require(accepted.status == no::SubmitStatus::Accepted && accepted.handle,
                        "native rootless opening accepted");
                tails_[slot] = *accepted.handle;
                cohorts_[slot] = cohort_open();
                cohort_add(cohorts_[slot], tails_[slot]);
            }
        } else if (index < kRootRows) {
            const auto slot = (index - 1) % tails_.size();
            replace_one(slot, false);
            if (index <= tails_.size()) oldest_[slot] = tails_[slot];
            if (index == 65 || index == 129) {
                // Both oldest aliases are terminal and now in the prefix.
                // Removing then re-enrolling the live descendant forces the
                // historical reader and preserves the logical cohort roster.
                for (std::size_t member = 0; member < tails_.size(); ++member) {
                    cohort_remove(cohorts_[member], oldest_[member]);
                    cohort_add(cohorts_[member], tails_[member]);
                }
            }
        } else if (index == kRootRows) {
            replace_one(0, true);
            require(cancel(tails_[1]).status == no::CancelStatus::Cancelled, "other chain cancelled");
        } else if (index == kRootRows + 1) {
            require(physical_position().signed_units == 1.0, "market successor actually filled");
            const auto close = submit(no::Request{no::Flatten{}, "close", ""});
            require(close.status == no::SubmitStatus::Accepted, "rootless close admitted at exact R");
        } else if (index == kRootRows + 2) {
            require(physical_position().signed_units == 0.0 && closed_trade_count() == 1,
                    "rootless close produced one real closed row");
        }
        callback_continuations.push_back(native_continuation_hash());
        callback_brokers.push_back(broker_state_hash());
    }

private:
    std::array<no::RequestHandle, 2> tails_{};
    std::array<no::RequestHandle, 2> oldest_{};
    std::array<no::CohortHandle, 2> cohorts_{};

    static no::Request opening(std::size_t slot, bool market) {
        no::Request value{no::Transact{1.0}, slot == 0 ? "A" : "B", ""};
        if (!market) value.trigger = no::Limit{90.0};
        return value;
    }

    void replace_one(std::size_t slot, bool market) {
        const auto result = replace(tails_[slot], opening(slot, market));
        require(result.status == no::ReplaceStatus::Replaced && result.successor,
                "native replacement accepted");
        tails_[slot] = *result.successor;
        ++replacements;
    }
};

void configure(RootsHost& host, bool bounded, std::uint64_t roots) {
    require(host.configure_native(spec()).status == NativeSetupStatus::Applied, "native spec accepted");
    if (bounded) {
        pf_scratch_tables_v1 descriptor{};
        descriptor.struct_size = static_cast<std::uint32_t>(sizeof(descriptor));
        descriptor.version = 1;
        descriptor.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
        descriptor.R = roots;
        require(host.set_scratch_tables_v1(&descriptor) == 0, "bounded root manifest accepted");
    }
    host.set_broker_state_hash_recording(true);
}

struct Report {
    ReportC value{};
    explicit Report(const RootsHost& host) { host.fill_report(&value); }
    ~Report() { BacktestEngine::free_report(&value); }
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
};

auto trade_key(const Trade& row) {
    return std::make_tuple(row.entry_time, row.exit_time, bits(row.entry_price), bits(row.exit_price),
            bits(row.qty), bits(row.pnl), bits(row.pnl_pct), row.is_long, row.entry_bar_index,
            row.exit_bar_index, row.entry_id, row.entry_comment, row.exit_comment, row.exit_id,
            row.exit_from_bracket, bits(row.max_runup), bits(row.max_drawdown), bits(row.commission),
            row.entry_incarnation, row.open_at_end, row.close_cause);
}

void disk_ram_run_identity() {
    const auto feed = bars();
    RootsHost disk;
    RootsHost ram;
    configure(disk, false, 0);
    configure(ram, true, kRootRows);
    disk.run(feed.data(), static_cast<int>(feed.size()));
    ram.run(feed.data(), static_cast<int>(feed.size()));
    require(disk.native_state().kind == NativeLifecycleKind::Completed, "disk root run completes");
    require(ram.native_state().kind == NativeLifecycleKind::Completed, "RAM root run completes");
    require(disk.begin_calls == 1 && ram.begin_calls == 1 && !disk.observed_slab && ram.observed_slab,
            "sink-OFF common begin selects the owner before the native callback");
    require(disk.bar_calls == kBars && ram.bar_calls == kBars
            && disk.replacements == kRootRows && ram.replacements == kRootRows, "all root work executed");
    require(disk.callback_continuations == ram.callback_continuations
            && disk.callback_brokers == ram.callback_brokers, "every callback preserves both digest reads");
    require(disk.native_continuation_hash() == ram.native_continuation_hash()
            && disk.broker_state_hash() == ram.broker_state_hash(), "final root folds are backend-independent");
    require(disk.closed_trade_count() == 1 && ram.closed_trade_count() == 1
            && trade_key(disk.closed_trade(0)) == trade_key(ram.closed_trade(0)),
            "all closed trade fields and floating bits match");
    const Report disk_report(disk);
    const Report ram_report(ram);
    const auto& left = disk_report.value;
    const auto& right = ram_report.value;
    require(left.broker_state_hash_len == static_cast<std::int64_t>(feed.size())
            && right.broker_state_hash_len == left.broker_state_hash_len
            && left.equity_curve_len == left.broker_state_hash_len
            && right.equity_curve_len == left.equity_curve_len, "real recorded report populations agree");
    require(left.total_trades == right.total_trades && left.trades_len == right.trades_len
            && bits(left.net_profit) == bits(right.net_profit), "report trade counts and net profit match");
    for (std::int64_t i = 0; i < left.broker_state_hash_len; ++i) {
        require(left.broker_state_hash[i] == right.broker_state_hash[i], "per-bar broker hash matches");
        require(left.equity_curve[i].time_ms == right.equity_curve[i].time_ms
                && bits(left.equity_curve[i].equity) == bits(right.equity_curve[i].equity)
                && bits(left.equity_curve[i].open_profit) == bits(right.equity_curve[i].open_profit),
                "every equity point and floating bit matches");
    }
    const auto& roots = ram.observed_slab->region(detail::ScratchResource::successor_roots);
    // Preparing row 129 flushes the second batch; row 130 remains inline.
    require(roots.capacity == 16 * kRootRows && roots.logical_extent == 2 * 64 * 16,
            "native run used exactly two fixed-prefix flushes");
}

void capacity_reaches_the_public_failure_channel(std::uint64_t roots) {
    const auto feed = bars();
    RootsHost host;
    configure(host, true, roots);
    host.run(feed.data(), static_cast<int>(feed.size()));
    require(host.native_state().kind == NativeLifecycleKind::Failed, "root overflow fails the native run");
    require(std::strcmp(run_failure_code_of(host), "scratch_capacity_exceeded") == 0
            && std::strcmp(run_failure_args_of(host), "{\"resource\":\"successor_roots\"}") == 0,
            "public failure preserves the root resource code");
    require(host.begin_calls == 1 && host.replacements == roots && host.closed_trade_count() == 0,
            "failing replacement publishes no successor or financial effect");
    // The two limits stop at the next full-tail preparation. A failed ticket
    // cannot flush that full inline batch into the already admitted region.
    const auto expected_prefix = 16 * 64 * ((roots - 1) / 64);
    require(host.observed_slab
            && host.observed_slab->region(detail::ScratchResource::successor_roots).logical_extent
                == expected_prefix, "run-level refusal precedes prefix mutation");
}

} // namespace

int main() {
    try {
        disk_ram_run_identity();
        for (const std::uint64_t roots : {64, 128}) capacity_reaches_the_public_failure_channel(roots);
        std::puts("native roots RAM run: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "native roots RAM run: %s\n", error.what());
        return 1;
    }
}
