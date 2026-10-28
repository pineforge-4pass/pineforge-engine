#pragma once

#include "../src/native_execution_consumer.hpp"
#include <pineforge/source/pine_strategy_host.hpp>
#include "native_order_full_fold.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace book_allowance_run {
namespace pf = pineforge;
namespace no = pineforge::native_order;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

inline std::vector<pf::Bar> tape(std::size_t count) {
    std::vector<pf::Bar> bars;
    bars.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        bars.push_back({100, 101, 99, 100, 1,
                       INT64_C(1736121600000) + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

class Host final : public pf::source::PineStrategyHost {
public:
    using Observer = std::function<void(Host&)>;
    explicit Host(bool reuse = true, Observer observer = {})
        : reuse_(reuse), observer_(std::move(observer)) {
        pf::source::PineStrategyConfig config;
        config.initial_capital = 1000000;
        config.default_qty_type = static_cast<int>(pf::QtyType::FIXED);
        config.default_qty_value = 1;
        config.pyramiding = 1000000;
        config.margin_long = config.margin_short = 100;
        configure_pine_strategy(config);
        // A nonzero fixed offset avoids host UTC TZif bytes in the run's hash.
        set_syminfo_timezone("UTC+01:00");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(false);
    }

    const no::WorkingRequestCore& core() {
        return pf::as_native_consumer(execution_consumer()).request_core();
    }
    void on_source_bar(const pf::Bar&) override {
        if (callbacks == 0) {
            pf::as_native_consumer(execution_consumer()).set_match_row_reuse(reuse_);
        }
        strategy_entry("L " + std::to_string(callbacks), true, 1,
                       std::numeric_limits<double>::quiet_NaN(), 1);
        ++callbacks;
        if (observer_) observer_(*this);
    }
    std::size_t callbacks = 0;
    std::size_t fills = 0;

protected:
    void on_native_applied_event(const no::ExecutionAppliedEvent&,
                                 const pf::NativeDecisionContext&) override {
        ++fills;
    }

private:
    bool reuse_;
    Observer observer_;
};

inline void finish(Host& host, std::size_t count) {
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.native_state().kind == pf::NativeLifecycleKind::Completed,
            "never-filled run completes");
    require(host.callbacks == count && host.core().live_size() == count,
            "one accepted live request per bar");
    require(host.trade_count() == 0 && host.report_trade_count() == 0 && host.fills == 0,
            "no closed trades, report trades or applied fills");
    require(host.physical_position().signed_units == 0, "never-filled book stays flat");
}

// This capture uses only APIs present before the refresh change. The lab
// compiles it against the unchanged engine to produce the reference bytes.
inline std::string transcript(std::size_t count, bool reuse = true) {
    Host host(reuse);
    const auto bars = tape(count);
    host.run(bars.data(), static_cast<int>(bars.size()));
    finish(host, count);
    full_fold::Fold rows;
    std::uint64_t previous = 0;
    for (const auto& row : host.core().live()) {
        require(row.priority() > previous, "working queue remains in priority order");
        previous = row.priority();
        full_fold::fold(rows, row);
    }
    char text[320];
    std::snprintf(text, sizeof text,
        "B=%zu callbacks=%zu live=%zu continuation=%016llx broker=%016llx "
        "rows=%016llx trades=%d report_trades=%d fills=%zu\n",
        count, host.callbacks, host.core().live_size(),
        static_cast<unsigned long long>(host.native_continuation_hash()),
        static_cast<unsigned long long>(host.broker_state_hash()),
        static_cast<unsigned long long>(rows.h), host.trade_count(),
        host.report_trade_count(), host.fills);
    return text;
}
} // namespace book_allowance_run
