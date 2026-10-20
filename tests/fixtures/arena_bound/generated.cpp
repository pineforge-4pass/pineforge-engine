// Deliberately small generated-code-style fixture, using the same source host
// and generated C entry points as tutorial/macd/generated.cpp.
// Inputs select adversarial schedules; no bound or arena size is read here.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/native_order.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
constexpr double no_price = std::numeric_limits<double>::quiet_NaN();
namespace no = pineforge::native_order;

no::Request resting_request() {
    no::Request request;
    request.intent = no::Transact{1.0};
    request.trigger = no::Limit{1.0};
    request.label = "roots";
    return request;
}

class ArenaBoundStrategy final : public pineforge::source::PineStrategyHost {
public:
    ArenaBoundStrategy() {
        fixture_config_.initial_capital = 1.0e12;
        fixture_config_.default_qty_value = 16.0;
        fixture_config_.pyramiding = 8;
        fixture_config_.process_orders_on_close = true;
        configure_pine_strategy(fixture_config_);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }

    void set_fixture_override(const std::string& key, const std::string& value) {
        if (key == "initial_capital") fixture_config_.initial_capital = std::stod(value);
        else if (key == "pyramiding") fixture_config_.pyramiding = std::stoi(value);
        else if (key == "default_qty_value") fixture_config_.default_qty_value = std::stod(value);
        else if (key == "process_orders_on_close")
            fixture_config_.process_orders_on_close = value == "true" || value == "1";
        else throw std::invalid_argument("unsupported fixture strategy override");
        configure_pine_strategy(fixture_config_);
    }

    void on_source_bar(const pineforge::Bar& current) override {
        peak_lots_ = std::max(peak_lots_,
            static_cast<std::uint64_t>(native_open_lots(current.close).size()));
        const int which = get_input_int("case", 0);
        const int bar = pine_bar_index();
        if (which == 0) return; // Million-point curve / undersizing control.
        if (which == 6) {
            if (replacements_ == 0) {
                const auto submitted = roots_.submit(
                    resting_request(), 0, incarnation_, ordinal_);
                if (!submitted.handle) throw std::runtime_error("root fixture submit failed");
                root_tail_ = *submitted.handle;
            }
            // Exercise actual new successor rows, not kept-handle repricing.
            // The independent core has no matching driver and never fills.
            for (int i = 0; i < 512; ++i) {
                const auto replaced = roots_.replace(
                    root_tail_, resting_request(), 0, incarnation_, ordinal_);
                if (replaced.status != no::ReplaceStatus::Replaced || !replaced.successor)
                    throw std::runtime_error("root fixture replacement failed");
                root_tail_ = *replaced.successor;
                ++replacements_;
                roots_.retire_history(roots_.last_ordinal());
            }
            return;
        }
        const int width = which == 1 || which == 5 ? 8 : (which == 4 ? 4 : 1);
        const int phase = bar % 8;
        const int cycle = bar / 8;
        const bool long_ids = which == 3;
        const bool recurring = which == 1 || which == 5;
        for (int i = 0; i < width; ++i) {
            const std::string id = (long_ids ? std::string(4096, 'x') : "E")
                + std::to_string(recurring ? 0 : cycle) + ":" + std::to_string(i);
            if (which == 1 || phase == 0) {
                strategy_entry(id, true, no_price, no_price, 16.0);
                ++entry_calls_;
            }
            if (which == 1 || (phase >= 1 && phase <= 5)) {
                // Three separately named partial brackets, each with a TP,
                // stop and trail candidate. Distinct pairs must survive erase.
                for (int part = 0; part < 3; ++part) {
                    const auto exit_id = "X" + std::to_string(part) + ":" + id;
                    strategy_exit(exit_id, id, 100.5 + part * 0.25, 98.0,
                                  4.0, 2.0, no_price, 25.0);
                    ++exit_calls_;
                }
            }
            if (phase == 5) strategy_close(id, "", 1.0);
            if (phase == 7) strategy_cancel(id);
        }
        if (which == 1 || phase == 6) strategy_close_all();
        if (phase == 7) strategy_cancel_all();
    }

    std::uint64_t entry_calls() const noexcept { return entry_calls_; }
    std::uint64_t exit_calls() const noexcept { return exit_calls_; }
    std::uint64_t replacements() const noexcept { return replacements_; }
    std::uint64_t peak_lots() const noexcept { return peak_lots_; }

private:
    pineforge::source::PineStrategyConfig fixture_config_{};
    no::WorkingRequestCore roots_{{"arena-bound-roots", 1}};
    no::RequestHandle root_tail_{};
    std::uint64_t incarnation_ = 1;
    std::uint64_t ordinal_ = 1;
    std::uint64_t entry_calls_ = 0;
    std::uint64_t exit_calls_ = 0;
    std::uint64_t replacements_ = 0;
    std::uint64_t peak_lots_ = 0;
};
} // namespace

extern "C" {
void* strategy_create(const char*) { return new ArenaBoundStrategy(); }
void strategy_free(void* state) { delete static_cast<ArenaBoundStrategy*>(state); }
void report_free(pineforge::ReportC* report) {
    pineforge::BacktestEngine::free_report(report);
}
void strategy_set_input(void* state, const char* key, const char* value) {
    static_cast<ArenaBoundStrategy*>(state)->set_input(key, value);
}
void strategy_set_override(void* state, const char* key, const char* value) {
    static_cast<ArenaBoundStrategy*>(state)->set_fixture_override(key, value);
}
void run_backtest(void* state, pineforge::Bar* bars, int count, pineforge::ReportC* report) {
    auto& strategy = *static_cast<ArenaBoundStrategy*>(state);
    strategy.run(bars, count);
    strategy.fill_report(report);
}
void run_backtest_full(void* state, pineforge::Bar* bars, int count,
                       const char* input_tf, const char* script_tf,
                       int magnifier, int samples, int distribution,
                       pineforge::ReportC* report) {
    auto& strategy = *static_cast<ArenaBoundStrategy*>(state);
    strategy.run(bars, count, input_tf ? input_tf : "", script_tf ? script_tf : "",
                 magnifier != 0, samples,
                 static_cast<pineforge::MagnifierDistribution>(distribution));
    strategy.fill_report(report);
}
std::uint64_t arena_bound_fixture_entries(void* state) {
    return static_cast<ArenaBoundStrategy*>(state)->entry_calls();
}
std::uint64_t arena_bound_fixture_exits(void* state) {
    return static_cast<ArenaBoundStrategy*>(state)->exit_calls();
}
std::uint64_t arena_bound_fixture_roots(void* state) {
    return static_cast<ArenaBoundStrategy*>(state)->replacements();
}
std::uint64_t arena_bound_fixture_peak_lots(void* state) {
    return static_cast<ArenaBoundStrategy*>(state)->peak_lots();
}
} // extern "C"
