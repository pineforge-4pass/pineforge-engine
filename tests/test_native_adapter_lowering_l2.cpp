// R4-D L2 fixture-host evidence. These scenarios drive the new source
// lowering through PineNativeHost; none derives the live PineStrategyHost or
// invokes a legacy pending-order route.
#include <pineforge/source/pine_native_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <type_traits>
#include <variant>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int checks = 0;
int failures = 0;
#define CHECK(expr) do {                                                         \
    ++checks;                                                                    \
    if (!(expr)) {                                                               \
        ++failures;                                                              \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);           \
    }                                                                            \
} while (0)

Bar bar(std::int64_t timestamp, double price = 100.0) {
    return {price, price, price, price, 1.0, timestamp};
}

int applied_with_label(const source::PineNativeHost& host, const char* label) {
    int result = 0;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* event = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
            if (event->request().label == label) ++result;
        }
    }
    return result;
}

class OneBarHost final : public source::PineNativeHost {
public:
    int callbacks = 0;
    bool saw_first_tick = false;
    bool saw_history_advance = false;
    void on_source_bar(const Bar&) override {
        ++callbacks;
        saw_first_tick = is_first_tick();
        saw_history_advance = history_advances_new_bar();
        if (pine_bar_index() == 0) strategy_entry("one", true, kNaN, kNaN, 2.0);
    }
private:
    static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
};

class EntryCloseHost final : public source::PineNativeHost {
public:
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("E", true, kNaN, kNaN, 2.0);
        else if (pine_bar_index() == 1) strategy_close("E", "partial", 1.0, kNaN, false);
        else if (pine_bar_index() == 2) strategy_close("E", "immediate", 1.0, kNaN, true);
    }
private:
    static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
};

class DeferredExitHost final : public source::PineNativeHost {
public:
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_exit("x", "E", 110.0, kNaN, kNaN, kNaN, kNaN, 100.0);
            strategy_entry("E", true, kNaN, kNaN, 2.0);
        }
    }
private:
    static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
};

class CoofFirstOpenHost final : public source::PineNativeHost {
public:
    CoofFirstOpenHost() {
        source::PineStrategyConfig config;
        config.calc_on_order_fills = true;
        config.pyramiding = 2;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("seed", true, kNaN, kNaN, 1.0);
        else if (pine_bar_index() == 1 && !born_) {
            born_ = true;
            strategy_entry("newborn", true, kNaN, kNaN, 1.0);
        }
    }
private:
    static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    bool born_ = false;
};

class ProjectionHost final : public source::PineNativeHost {
public:
    ProjectionHost() {
        source::PineStrategyConfig config;
        config.process_orders_on_close = true;
        config.initial_capital = 2500.0;
        config.default_qty_type = static_cast<int>(QtyType::CASH);
        config.default_qty_value = 125.0;
        config.pyramiding = 3;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 6.0;
        config.slippage = 2;
        config.margin_long = 50.0;
        config.margin_short = 25.0;
        configure_pine_strategy(config);
    }
    std::string seen_tickerid;
    void on_source_bar(const Bar&) override { seen_tickerid = syminfo_.tickerid; }
};

void undetected_one_bar_provider_witness() {
    OneBarHost host;
    const Bar bars[] = {bar(12345)}; // L1b timestamp partition, deliberately off grid.
    host.run(bars, 1);
    const auto state = host.native_state();
    CHECK(state.kind == NativeLifecycleKind::Completed);
    CHECK(state.spec && state.spec->timeframe_undetected);
    CHECK(state.spec && state.spec->input_tf.empty() && state.spec->script_tf.empty());
    CHECK(host.callbacks == 1 && host.saw_first_tick && host.saw_history_advance);
    CHECK(host.pending_order_count() == 1 && applied_with_label(host, "one") == 0);
    double qty = 7.0; int close_only = 7; int partition = 7;
    CHECK(host.probe_fill_qty(-1, 100.0, &qty, &close_only, &partition) == -1);
    CHECK(qty == 7.0 && close_only == 7 && partition == 7);
    CHECK(host.probe_fill_qty(0, 100.0, &qty, &close_only, &partition) == 0);
    CHECK(std::abs(qty - 2.0) < 1e-12 && close_only == 0);
}

void command_lowering_and_current_execution_witness() {
    EntryCloseHost host;
    const Bar bars[] = {bar(60000), bar(120000), bar(180000), bar(240000)};
    host.run(bars, 4);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(applied_with_label(host, "E") >= 1);
    CHECK(host.trade_count() >= 1);
    CHECK(host.broker_state_hash() != 0);
}

void deferred_cohort_exit_witness() {
    DeferredExitHost host;
    const Bar bars[] = {bar(60000), bar(120000), bar(180000, 110.0), bar(240000, 110.0)};
    host.run(bars, 4);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(applied_with_label(host, "E") >= 1);
    bool saw_exit = false;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        std::visit([&](const auto& event) {
            using T = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<T, no::AcceptedEvent> || std::is_same_v<T, no::ExecutionAppliedEvent>) {
                if (event.request().label == "x") saw_exit = true;
            }
        }, *row.command);
    }
    CHECK(saw_exit);
}

void coof_first_open_current_execution_witness() {
    CoofFirstOpenHost host;
    const Bar bars[] = {bar(60000), bar(120000), bar(180000)};
    host.run(bars, 3);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(applied_with_label(host, "seed") == 1);
    // P7d: first-open recalculation's newborn market executes at that same open.
    CHECK(applied_with_label(host, "newborn") == 1);
    CHECK(host.physical_position().lot_count == 2);
}

void provider_projection_and_rich_syminfo_witness() {
    ProjectionHost host;
    SymInfo rich;
    rich.ticker = "RICH";
    rich.tickerid = "RICH:SYMINF0";
    rich.type = "futures";
    rich.currency = "USD";
    rich.basecurrency = "R";
    rich.description = "rich fixture";
    rich.volumetype = "contracts";
    rich.timezone = "Asia/Taipei";
    rich.session = "0900-1330";
    rich.mintick = 0.25;
    rich.pointvalue = 5.0;
    rich.qty_step = 0.5;
    const Bar bars[] = {bar(1736121600000LL), bar(1736121660000LL)};
    InputsMap inputs{{"mode", "rich"}};
    host.run(bars, 2, "1", "1", inputs, rich);
    const auto state = host.native_state();
    CHECK(state.kind == NativeLifecycleKind::Completed && state.spec);
    if (!state.spec) return;
    CHECK(!state.spec->timeframe_undetected);
    CHECK(state.spec->input_tf == "1" && state.spec->script_tf == "1");
    CHECK(state.spec->tickerid == "RICH:SYMINF0" && host.seen_tickerid == "RICH:SYMINF0");
    CHECK(state.spec->timezone == "Asia/Taipei" && state.spec->session == "0900-1330");
    CHECK(state.spec->point_value == 5.0 && state.spec->price_tick == 0.25);
    CHECK(state.spec->quantity_grid && *state.spec->quantity_grid == 0.5);
    rich.tickerid = "MUTATED";
    CHECK(state.spec->tickerid == "RICH:SYMINF0");
}

} // namespace

int main() {
    undetected_one_bar_provider_witness();
    command_lowering_and_current_execution_witness();
    deferred_cohort_exit_witness();
    coof_first_open_current_execution_witness();
    provider_projection_and_rich_syminfo_witness();
    std::printf("R4-D L2 native adapter fixture: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
