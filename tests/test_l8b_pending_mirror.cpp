#include <pineforge/pineforge.h>
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

namespace {
bool deny_allocation = false;
std::size_t denied_allocations = 0;
}

void* operator new(std::size_t size) {
    if (deny_allocation) {
        ++denied_allocations;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
// libstdc++'s temporary buffers (std::stable_sort) allocate through the
// nothrow forms and release through the sized delete above; replacing only
// the throwing forms mixes the real operator new with free (ASan
// alloc-dealloc-mismatch on the sanitizers lane).
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (deny_allocation) { ++denied_allocations; return nullptr; }
    return std::malloc(size);
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }

using namespace pineforge;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int checks = 0;
int failures = 0;

#define CHECK(expression) do { ++checks; if (!(expression)) { \
    ++failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", \
                             __FILE__, __LINE__, #expression); } } while (false)

class LifecycleMirrorRoute final : public source::PineStrategyHost {
public:
    LifecycleMirrorRoute() {
        source::PineStrategyConfig config;
        config.initial_capital = 10'000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.pyramiding = 1;
        configure_pine_strategy(config);
        set_margin_call_enabled(false);
        set_syminfo_mintick(0.01);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 1) {
            strategy_exit("X", "L", kNaN, 90.0, 7.54, 1.0);
            strategy_entry("S", false);
        }
        if (bar_index_ != 2 || captured_) return;
        const auto& view = pending_intent_view();
        for (int index = 0; index < view.size(); ++index) {
            pf_pending_order_v1_t candidate{};
            if (view.copy_v1(index, &candidate) == 0
                && std::strcmp(candidate.id, "X") == 0
                && candidate.legs_suspension_present == 1U) {
                row_ = candidate;
                captured_ = true;
                break;
            }
        }
    }

    bool captured() const noexcept { return captured_; }
    const pf_pending_order_v1_t& row() const noexcept { return row_; }

private:
    bool captured_ = false;
    pf_pending_order_v1_t row_{};
};

class SignalCloseMarginRoute final : public source::PineStrategyHost {
public:
    SignalCloseMarginRoute() {
        source::PineStrategyConfig config;
        config.initial_capital = 1'045'584.2231012;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.margin_long = config.margin_short = 100.0;
        config.pyramiding = 1;
        config.process_orders_on_close = false;
        configure_pine_strategy(config);
        qty_step_ = 0.01;
        syminfo_.pointvalue = 1.0;
        set_syminfo_mintick(0.00001);
        set_margin_call_enabled(true);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 888'241.18);
        if (bar_index_ == 1)
            strategy_entry("S", false, kNaN, kNaN, kNaN, "REV");
    }

};

class TargetedCloseRoute final : public source::PineStrategyHost {
public:
    TargetedCloseRoute() {
        source::PineStrategyConfig config;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, 1.0);
        if (bar_index_ == 1) strategy_close("L");
    }
};

void lifecycle_variant_projection() {
    LifecycleMirrorRoute route;
    const Bar bars[] = {
        {100, 100, 100, 100, 1, 1'000},
        {100, 112, 99, 110, 1, 2'000},
        {111, 112, 100, 111, 1, 3'000},
        {111, 111, 111, 111, 1, 4'000},
    };
    route.run(bars, 4);
    CHECK(route.last_error().empty());
    CHECK(route.captured());
    if (!route.captured()) return;
    const auto& row = route.row();
    CHECK(row.legs_last_operation == 1U);
    CHECK(row.legs_last_suspend_legs_count == 2U);
    CHECK(row.legs_last_suspend_legs_item0
          == static_cast<std::uint32_t>(exit_legs::Leg::Stop));
    CHECK(row.legs_last_suspend_legs_item1
          == static_cast<std::uint32_t>(exit_legs::Leg::Limit));
    CHECK(row.legs_last_suspend_window_present == 1U);
    CHECK(row.legs_last_suspend_window_excluded_event == row.legs_last_cause_event);
    CHECK(row.legs_last_suspend_retire_count == 1U);
    CHECK(row.legs_last_suspend_retire_item0
          == static_cast<std::uint32_t>(exit_legs::Leg::Trail));
}

void signal_close_margin_projection() {
    SignalCloseMarginRoute route;
    const Bar bars[] = {
        {1.17714, 1.17714, 1.17714, 1.17714, 1, 1'000},
        {1.17714, 1.17746, 1.17652, 1.17653, 1, 2'000},
    };
    route.run(bars, 2);
    CHECK(route.last_error().empty());
    CHECK(route.pending_order_count() == 1);
    pf_pending_order_v1_t row{};
    CHECK(strategy_pending_order_get(&route, 0, &row, sizeof(row)) == 0);
    CHECK(row.rounded_signal_cost_close_only == 1U);
    CHECK(row.signal_close_mc_bar == 1);
    CHECK(row.signal_close_mc_entry_incarnation != 0U);
    CHECK(row.signal_close_mc_fill_seq != 0U);
    CHECK(std::abs(row.signal_close_mc_remaining_qty - 888'240.18) < 1e-6);
}

void targeted_close_copy_is_allocation_free() {
    TargetedCloseRoute route;
    const Bar bars[] = {
        {100, 100, 100, 100, 1, 1'000},
        {100, 100, 100, 100, 1, 2'000},
    };
    route.run(bars, 2);
    CHECK(route.last_error().empty());
    CHECK(route.pending_order_count() == 1);
    pf_pending_order_v1_t row{};
    const auto before = denied_allocations;
    deny_allocation = true;
    const int status = strategy_pending_order_get(&route, 0, &row, sizeof(row));
    deny_allocation = false;
    CHECK(status == 0);
    CHECK(denied_allocations == before);
    CHECK(std::strcmp(row.id, "__close__L") == 0);
    CHECK(row.pine_frozen_market_instruction_target_id[0] != '\0');
}
} // namespace

int main() {
    lifecycle_variant_projection();
    signal_close_margin_projection();
    targeted_close_copy_is_allocation_free();
    std::printf("L8b pending mirror: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
