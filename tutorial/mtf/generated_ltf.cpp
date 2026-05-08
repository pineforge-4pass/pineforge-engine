// PineForge tutorial — MTF (lower-TF) generated.cpp.
//
// Mirrors strategy_ltf.pine. Demonstrates the request.security_lower_tf
// codegen contract:
//
//   1. Constructor calls register_security_lower_tf_eval(sec_id,
//      requested_tf, input_tf). The registration helper records that
//      this site wants per-sub-bar dispatch instead of the standard
//      complete/partial cadence.
//
//   2. evaluate_security() is invoked once per synthetic sub-bar with
//      is_complete=true. The engine resets security_lower_tf_sub_bar_index
//      to 0 at the start of each chart bar and increments it after every
//      dispatch — codegen detects index==0 to clear its accumulator.
//
//   3. on_bar() reads the accumulated vector — that is the Pine array
//      that request.security_lower_tf returns.
//
// Engine-side pipeline lives in src/engine_security.cpp (registration +
// validation) and src/engine_lower_tf.cpp (sub-bar synthesis from the
// input bar's OHLC path).
#include <pineforge/engine.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/math.hpp>
#include <pineforge/series.hpp>
#include <pineforge/na.hpp>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <string>
#include <vector>
#include <tuple>
#include <memory>
#include <unordered_map>
#include <pineforge/color.hpp>
#include <pineforge/log.hpp>
#include <pineforge/str_utils.hpp>
#include <pineforge/session_time.hpp>

using namespace pineforge;

class GeneratedStrategy : public BacktestEngine {
public:
    // The accumulated vector of per-sub-bar closes for the current
    // chart bar. Cleared at sub-bar index 0, pushed at every dispatch,
    // read in on_bar().
    std::vector<double> _req_sec_lower_tf_0{};

    explicit GeneratedStrategy() {
        initial_capital_ = 1000000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 1;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        slippage_ = 0;
        script_has_strategy_close_ = true;
    }

    void set_strategy_override(const std::string& key, const std::string& value) {
        if (key == "initial_capital") { initial_capital_ = std::stod(value); return; }
        if (key == "commission_value") { commission_value_ = std::stod(value); return; }
        if (key == "default_qty_value") { default_qty_value_ = std::stod(value); return; }
        if (key == "pyramiding") { pyramiding_ = std::stoi(value); return; }
        if (key == "slippage") { slippage_ = std::stoi(value); return; }
        if (key == "process_orders_on_close") { process_orders_on_close_ = (value == "true" || value == "1"); return; }
        if (key == "close_entries_rule") { close_entries_rule_any_ = (value == "ANY" || value == "any" || value == "1"); return; }
        if (key == "default_qty_type") {
            if (value == "fixed" || value == "strategy.fixed" || value == "0") default_qty_type_ = QtyType::FIXED;
            else if (value == "percent_of_equity" || value == "strategy.percent_of_equity" || value == "1") default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
            else if (value == "cash" || value == "strategy.cash" || value == "2") default_qty_type_ = QtyType::CASH;
            return;
        }
        if (key == "commission_type") {
            if (value == "percent" || value == "strategy.commission.percent" || value == "0") commission_type_ = CommissionType::PERCENT;
            else if (value == "cash_per_order" || value == "strategy.commission.cash_per_order" || value == "1") commission_type_ = CommissionType::CASH_PER_ORDER;
            else if (value == "cash_per_contract" || value == "strategy.commission.cash_per_contract" || value == "2") commission_type_ = CommissionType::CASH_PER_CONTRACT;
            return;
        }
    }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        // Target "1" minute. The engine validates that input_tf_ is a
        // clean integer multiple of this and is strictly coarser; if
        // input_tf_ is "15", the engine synthesizes 15 sub-bars per
        // chart bar from each chart bar's OHLC path.
        register_security_lower_tf_eval(0, std::string("1"), input_tf_);
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) {
            return;
        }
        if (security_lower_tf_sub_bar_index(0) == 0) {
            _req_sec_lower_tf_0.clear();
        }
        _req_sec_lower_tf_0.push_back(bar.close);
    }

    void clear_security(int sec_id) override {
        if (sec_id == 0) {
            _req_sec_lower_tf_0.clear();
        }
    }

    void on_bar(const Bar& bar) override {
        double rangePct = get_input_double("Range threshold (% of bar close)", 0.5);

        double subHi = na<double>();
        double subLo = na<double>();
        if (!_req_sec_lower_tf_0.empty()) {
            subHi = *std::max_element(_req_sec_lower_tf_0.begin(),
                                       _req_sec_lower_tf_0.end());
            subLo = *std::min_element(_req_sec_lower_tf_0.begin(),
                                       _req_sec_lower_tf_0.end());
        }

        double pct = (std::isnan(subHi) || std::isnan(subLo) || bar.close == 0.0)
                     ? 0.0
                     : (subHi - subLo) / bar.close * 100.0;

        bool longCond = (pct > rangePct) && (bar.close > bar.open) && (signed_position_size() == 0);
        bool exitCond = (signed_position_size() > 0) && (bar.close < bar.open);

        if (longCond) {
            strategy_entry(std::string("Long"), true,
                           na<double>(), na<double>(), 1, "", "", 0, -1);
        }
        if (exitCond) {
            strategy_close(std::string("Long"), "", na<double>(), na<double>(), false);
        }
    }
};

extern "C" {
    void* strategy_create(const char* /*params_json*/) {
        return new GeneratedStrategy();
    }
    void run_backtest(void* s, Bar* bars, int n, ReportC* out) {
        auto* strat = static_cast<GeneratedStrategy*>(s);
        strat->run(bars, n, "", "", false, 4, MagnifierDistribution::ENDPOINTS);
        strat->fill_report(out);
    }
    void run_backtest_full(void* s, Bar* bars, int n,
                           const char* input_tf, const char* script_tf,
                           int bar_magnifier, int magnifier_samples,
                           int magnifier_dist,
                           ReportC* out) {
        auto* strat = static_cast<GeneratedStrategy*>(s);
        std::string itf = input_tf ? input_tf : "";
        std::string stf = script_tf ? script_tf : "";
        strat->run(bars, n, itf, stf, bar_magnifier != 0, magnifier_samples,
                   static_cast<MagnifierDistribution>(magnifier_dist));
        strat->fill_report(out);
    }
    void strategy_free(void* s) {
        delete static_cast<GeneratedStrategy*>(s);
    }
    void report_free(ReportC* report) {
        BacktestEngine::free_report(report);
    }
    void strategy_set_input(void* s, const char* key, const char* value) {
        if (!s || !key || !value) return;
        static_cast<GeneratedStrategy*>(s)->set_input(key, value);
    }
    void strategy_set_override(void* s, const char* key, const char* value) {
        if (!s || !key || !value) return;
        static_cast<GeneratedStrategy*>(s)->set_strategy_override(key, value);
    }
    void strategy_set_magnifier_volume_weighted(void* s, int on) {
        if (!s) return;
        static_cast<GeneratedStrategy*>(s)->set_magnifier_volume_weighted(on != 0);
    }
}
