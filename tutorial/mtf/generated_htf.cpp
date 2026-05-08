// PineForge tutorial — MTF (HTF filter) generated.cpp.
//
// Mirrors strategy_htf.pine: MACD entries on the chart TF, gated by an
// HTF SMA trend filter brought in via two request.security calls.
//
// Two security evaluators are registered in configure_security_evaluators
// (one per HTF series): the HTF can be changed at runtime via
// strategy_set_input("HTF", "240") because configure_security_evaluators
// runs *after* set_input but *before* the bar loop — see the dispatch
// order in src/engine_run.cpp around line 174.
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
    // HTF-side state (one per request.security call).
    double _req_sec_0 = na<double>();   // HTF ta.sma(close, smaLen)
    double _req_sec_1 = na<double>();   // HTF close
    ta::SMA _sec0_sma;
    Series<double> htfSma;
    Series<double> htfClose;

    // Chart-TF state.
    ta::MACD _ta_macd;
    ta::Crossover _ta_crossover;
    ta::Crossunder _ta_crossunder;
    bool _ta_initialized_ = false;

    explicit GeneratedStrategy()
        : _sec0_sma(20),
          _ta_macd(12, 26, 9) {
        initial_capital_ = 1000000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 1;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        slippage_ = 0;
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
        // HTF string is read from inputs at run time. Default "60".
        std::string htf = get_input_string("HTF", std::string("60"));
        register_security_eval(0, htf, input_tf_, false, false);
        register_security_eval(1, htf, input_tf_, false, false);
        // Re-seed the HTF SMA with the (possibly swept) length.
        _sec0_sma = ta::SMA(get_input_int("SMA Length", 20));
    }

    void on_bar(const Bar& bar) override {
        if (!_ta_initialized_) {
            _ta_macd = ta::MACD(get_input_int("Fast Length", 12),
                                 get_input_int("Slow Length", 26),
                                 get_input_int("Signal Length", 9));
            _ta_initialized_ = true;
        }
        htfSma.push(_req_sec_0);
        htfClose.push(_req_sec_1);

        auto m = (is_first_tick_ ? _ta_macd.compute(bar.close)
                                 : _ta_macd.recompute(bar.close));
        bool xup = (is_first_tick_ ? _ta_crossover.compute(m.macd_line, m.signal_line)
                                   : _ta_crossover.recompute(m.macd_line, m.signal_line));
        bool xdn = (is_first_tick_ ? _ta_crossunder.compute(m.macd_line, m.signal_line)
                                   : _ta_crossunder.recompute(m.macd_line, m.signal_line));

        bool trendUp = !std::isnan(htfClose[0]) && !std::isnan(htfSma[0])
                       && (htfClose[0] > htfSma[0]);

        if (xup && trendUp) {
            strategy_entry(std::string("Long"), true,
                           na<double>(), na<double>(), na<double>(), "");
        }
        if (xdn && !trendUp) {
            strategy_entry(std::string("Short"), false,
                           na<double>(), na<double>(), na<double>(), "");
        }
    }

    void _eval_security_0(const Bar& bar, bool is_complete) {
        _req_sec_0 = is_complete ? _sec0_sma.compute(bar.close)
                                 : _sec0_sma.recompute(bar.close);
    }

    void _eval_security_1(const Bar& bar, bool /*is_complete*/) {
        _req_sec_1 = bar.close;
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        switch (sec_id) {
            case 0: _eval_security_0(bar, is_complete); break;
            case 1: _eval_security_1(bar, is_complete); break;
        }
    }

    void clear_security(int sec_id) override {
        switch (sec_id) {
            case 0: _req_sec_0 = na<double>(); break;
            case 1: _req_sec_1 = na<double>(); break;
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
