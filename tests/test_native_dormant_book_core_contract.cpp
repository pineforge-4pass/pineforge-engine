// Resident dormant sweeps must advance the request epoch twice per live row.
// Public run/stream/command calls cover an entirely resident book and removal
// of the final spilled row. A read-only internal epoch observation through
// PINEFORGE_TEST_DORMANT_CORE_EPOCH(NativeExecutionConsumer&) detects skipped
// refreshes or a stale spilled-class bump without extending public headers or state.
#include "../src/native_execution_consumer.hpp"
#include <pineforge/native_host.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>

#ifndef PINEFORGE_TEST_DORMANT_CORE_EPOCH
#error "resident epoch check requires PINEFORGE_TEST_DORMANT_CORE_EPOCH to read the request core epoch"
#else
namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
constexpr std::int64_t start = 1736121600000LL;

void require(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
}

class Host final : public NativeStrategyHost {
public:
    Host(std::size_t rows, bool spill) : rows_(rows), spill_(spill) {}
    bool woke = false;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (++bar_ == 1) {
            for (std::size_t i = 0; i < rows_; ++i) {
                const bool wake = spill_ && i + 1 == rows_;
                no::Request r{no::Transact{1}, wake ? "epoch-wake" : "epoch-rest", ""};
                r.trigger = no::Limit{wake ? 99.0 : 50.0};
                require(submit(r).handle.has_value(), "resident epoch initial request placement refused");
            }
        } else if (bar_ == 2 && spill_) {
            no::Request r{no::Transact{1}, "epoch-extra", ""};
            r.trigger = no::Limit{50};
            const auto result = submit(r);
            require(result.handle.has_value(), "resident epoch extra request placement refused");
            extra_ = *result.handle;
        }
    }
    void on_native_applied(const no::ExecutionAppliedEvent& e,
                           const NativeDecisionContext&) override {
        if (e.definition->request.label != "epoch-wake") return;
        // Wake's promotion first spills extra; its fill leaves a vacancy,
        // so cancelling extra promotes the LAST spilled dormant row without
        // spilling another. The following point is entirely resident.
        require(cancel(extra_).status == no::CancelStatus::Cancelled,
                "resident epoch final spilled request cancellation refused");
        woke = true;
    }
private:
    std::size_t rows_;
    bool spill_;
    int bar_ = 0;
    no::RequestHandle extra_;
};

void run(std::size_t rows, bool spill) {
    Host h(rows, spill);
    NativeRunSpec s;
    s.identity = {"c3-dormant-core-contract", 1};
    s.input_tf = "5"; s.script_tf = "5";
    s.tickerid = "TEST:C3"; s.timezone = "UTC"; s.session = "24x7";
    s.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    s.initial_capital = 1.0e9; s.point_value = 1; s.account_fx = 1;
    s.report_policy = NativeReportPolicy::HostRecorded;
    require(h.configure_native(s).status == NativeSetupStatus::Applied, "resident epoch run configuration refused");
    std::array<Bar, 3> warmup{};
    for (std::size_t i = 0; i < warmup.size(); ++i)
        warmup[i] = {100, 100, 100, 100, 1,
                     start + static_cast<std::int64_t>(i) * 300000};
    require(h.stream_begin(warmup.data(), static_cast<int>(warmup.size()), "5", "5"),
            "resident epoch stream warmup failed");
    auto& c = h.consumer();
    std::uint64_t sequence = 1;
    if (spill) {
        require(c.request_core().book_spilled(), "final spilled row fixture never spilled");
        require(h.stream_push_tick({start + 900000, sequence++, 98, 1}),
                "resident epoch wake tick failed");
        require(h.woke && !c.request_core().book_spilled(),
                "resident epoch wake did not remove the final spilled dormant row");
        require(c.request_core().live_size() == rows - 1, "resident epoch wake left an unexpected book size");
    }
    const auto count = c.request_core().live_size();
    require(c.dormant_rows() > 0 && c.fast_points() > 0 && c.eager_points() == 0,
            "resident epoch run never exercised dormant resident sweeps");
    const auto before = PINEFORGE_TEST_DORMANT_CORE_EPOCH(c);
    require(h.stream_push_tick({start + 900001, sequence, 100, 1}), "resident epoch observation tick failed");
    const auto after = PINEFORGE_TEST_DORMANT_CORE_EPOCH(c);
    // Hand arithmetic: one new point, count unchanged resident Limit rows;
    // each eager refresh bumps exactly two, with no spilled-class bump.
    // The delta detects both a skipped refresh and a class bump left behind
    // after the last spilled row was promoted or cancelled.
    require(after - before == 2 * count, "resident epoch delta is not two per live row");
    require(c.request_core().live_size() == count, "resident epoch observation point changed membership");
    require(h.stream_end(false), "resident epoch stream completion failed");
}
} // namespace

int main() {
    try {
        run(512, false);
        run(4096, false);
        run(4096, true);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "test_native_dormant_book_core_contract: %s\n", e.what());
        return 1;
    }
}
#endif
