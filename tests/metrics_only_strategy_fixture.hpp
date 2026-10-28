#pragma once

#include <pineforge/native_module.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace metrics_only_fixture {

inline constexpr std::int64_t kEpochMs = 1514764800000LL; // 2018-01-01 UTC
inline constexpr std::int64_t kDayMs = 86400000LL;

// Both modules inherit precisely this trading, marking and presentation code.
// There are thirteen scheduled orders over the test's six-year interval,
// regardless of bar density; strategy and trade history cannot mask curve RSS.
class TradingHost : public pineforge::NativeStrategyHost {
public:
    TradingHost() {
        pineforge::NativeRunSpec spec;
        spec.identity = {"metrics-only-fixture", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.ticker = "METRICS";
        spec.tickerid = "TEST:METRICS";
        spec.timezone = spec.chart_timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 100000.;
        spec.point_value = spec.account_fx = 1.;
        spec.price_tick = .01;
        spec.fee_kind = pineforge::NativeFeeKind::Percent;
        spec.fee_value = .07;
        // Sparse minute labels are intentional: the small tape has one
        // supplied bar per day, with no aggregation or invented gap bars.
        spec.slot_label_policy = pineforge::NativeSlotLabelPolicy::FeedTolerant;
        spec.report_policy = pineforge::NativeReportPolicy::KernelRecordedAtHostMarks;
        spec.report_open_position_at_end = true;
        if (configure_native(spec).status != pineforge::NativeSetupStatus::Applied)
            throw std::logic_error("metrics fixture setup refused");
    }

    ~TradingHost() override {
        if (!std::getenv("PINEFORGE_METRICS_ONLY_TEST_RECEIPT")) return;
        // Observe only after the real runner has finished with this handle.
        // Use the noexcept production getter; never assemble another report,
        // alter preferences, admit a run or allocate a second curve here.
        pineforge::metrics::MetricsExtentReceiptV1 receipt{};
        const auto status = strategy_metrics_extent_receipt_v1(
            static_cast<const pineforge::BacktestEngine*>(this), &receipt);
        char last_equity[32] = "null";
        if (receipt.actual_last_equity_valid == 1 && std::isfinite(receipt.actual_last_equity))
            std::snprintf(last_equity, sizeof(last_equity), "%.17g", receipt.actual_last_equity);
        std::fprintf(stderr,
            "metrics-only-native {\"status\":%d,\"route\":%u,\"reason\":%u,"
            "\"mode\":%u,\"mask\":%llu,\"plain\":%u,\"failed\":%llu,"
            "\"witness\":%u,\"mismatch\":%llu,\"replays\":%u,"
            "\"planned\":%u,\"count\":%lld,\"first\":%lld,\"last\":%lld,"
            "\"last_equity_valid\":%u,\"last_equity\":%s,"
            "\"attempt\":%llu,\"authoritative_attempt\":%llu}\n",
            static_cast<int>(status), static_cast<unsigned>(receipt.route),
            static_cast<unsigned>(receipt.route_reason),
            static_cast<unsigned>(receipt.streaming_sharpe),
            static_cast<unsigned long long>(receipt.metrics_needed),
            static_cast<unsigned>(receipt.plain_subset),
            static_cast<unsigned long long>(receipt.failed_plain_inputs),
            static_cast<unsigned>(receipt.witness),
            static_cast<unsigned long long>(receipt.mismatch_fields),
            static_cast<unsigned>(receipt.replay_attempts),
            static_cast<unsigned>(receipt.extent_plan.present),
            static_cast<long long>(receipt.actual_extent.count),
            static_cast<long long>(receipt.actual_extent.first_time_ms),
            static_cast<long long>(receipt.actual_extent.last_time_ms),
            static_cast<unsigned>(receipt.actual_last_equity_valid), last_equity,
            static_cast<unsigned long long>(receipt.actual_attempt),
            static_cast<unsigned long long>(receipt.authoritative_attempt));
    }

    void on_native_run_begin() override { last_bucket_ = -1; }

    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext&) override {
        const std::int64_t bucket = (bar.timestamp - kEpochMs) / (180 * kDayMs);
        if (bucket != last_bucket_) {
            switch (bucket % 4) {
            case 0:
                (void)submit({pineforge::native_order::Transact{100.}, "long", ""});
                break;
            case 1:
                (void)submit({pineforge::native_order::Transact{-100.}, "close-long", ""});
                break;
            case 2:
                (void)submit({pineforge::native_order::Transact{-60.}, "short", ""});
                break;
            case 3:
                (void)submit({pineforge::native_order::Transact{60.}, "close-short", ""});
                break;
            }
            last_bucket_ = bucket;
        }
        // Exactly one builtin report mark per supplied input, including the
        // first and final bars. There is no anchor or custom presenter.
        if (!mark_native_report_point(bar.timestamp))
            throw std::logic_error("metrics fixture mark refused");
    }

protected:
    void hash_host_extension(pineforge::BrokerStateHashSink& sink) const override {
        pineforge::BacktestEngine::hash_host_extension(sink);
        sink.s("metrics-only-fixture/v1");
        sink.i(last_bucket_);
    }

private:
    std::int64_t last_bucket_ = -1;
};

} // namespace metrics_only_fixture
