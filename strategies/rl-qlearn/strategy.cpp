// strategies/rl-qlearn/strategy.cpp — hand-written reinforcement-learning
// demo strategy (tabular Q-learning, online).
//
// Unlike corpus/tutorial strategies this file is NOT transpiled from
// PineScript: PineScript has no mutable-2D-array-friendly RL idiom, so the
// agent is written directly against the BacktestEngine API and exports the
// same C ABI surface (strategy_create / run_backtest_full / ...) the Python
// harnesses expect.
//
// Agent design
// ------------
//   State  (60 discrete states): RSI(14) bucket [5] x EMA(10)>EMA(40) trend
//          flag [2] x 1-bar momentum bucket [3] x ATR%-vs-baseline volatility
//          regime [2].
//   Action (3): FLAT, LONG, SHORT — executed as strategy_close_all() /
//          strategy_entry(); a reversal is a single entry (TV semantics).
//   Reward: direction held during the elapsed bar x bar log-return (in %),
//          minus a switch penalty when the previous decision changed the
//          target position (proxy for spread/slippage round-trip cost).
//   Update: Q(s,a) += alpha * (r + gamma * max_a' Q(s',a') - Q(s,a)),
//          epsilon-greedy with multiplicative epsilon decay, seeded
//          xorshift64* PRNG so every run is bit-reproducible.
//
// The agent learns online during the single backtest pass — early trades are
// exploration; the runner reports first-half vs second-half PnL to show the
// effect of learning.

#include <pineforge/engine.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

using namespace pineforge;

namespace rlq {

// Deterministic xorshift64* PRNG — no <random> so runs are reproducible
// across platforms/standard libraries.
class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ULL) {}

    uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1DULL;
    }

    // Uniform double in [0, 1).
    double uniform() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }

    // Uniform int in [0, n).
    int uniform_int(int n) { return (int)(uniform() * (double)n) % n; }

private:
    uint64_t state_;
};

}  // namespace rlq

class RLQLearnStrategy : public BacktestEngine {
public:
    static constexpr int kActFlat  = 0;
    static constexpr int kActLong  = 1;
    static constexpr int kActShort = 2;
    static constexpr int kNumActions = 3;

    // 5 RSI buckets x 2 trend x 3 momentum x 2 volatility regimes.
    static constexpr int kNumStates = 5 * 2 * 3 * 2;

    explicit RLQLearnStrategy()
        : rsi_(14), ema_fast_(10), ema_slow_(40), atr_(14), atr_baseline_(96),
          rng_(20240607ULL) {
        initial_capital_   = 1000000.0;
        default_qty_type_  = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_        = 1;
        commission_type_   = CommissionType::PERCENT;
        commission_value_  = 0.0;
        slippage_          = 0;
        for (auto& row : q_) row.fill(0.0);
    }

    void set_strategy_override(const std::string& key, const std::string& value) {
        if (key == "initial_capital")   { initial_capital_   = std::stod(value); return; }
        if (key == "commission_value")  { commission_value_  = std::stod(value); return; }
        if (key == "default_qty_value") { default_qty_value_ = std::stod(value); return; }
        if (key == "pyramiding")        { pyramiding_        = std::stoi(value); return; }
        if (key == "slippage")          { slippage_          = std::stoi(value); return; }
        if (key == "process_orders_on_close") {
            process_orders_on_close_ = (value == "true" || value == "1"); return;
        }
        if (key == "default_qty_type") {
            if (value == "fixed" || value == "strategy.fixed" || value == "0")
                default_qty_type_ = QtyType::FIXED;
            else if (value == "percent_of_equity" || value == "strategy.percent_of_equity" || value == "1")
                default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
            else if (value == "cash" || value == "strategy.cash" || value == "2")
                default_qty_type_ = QtyType::CASH;
            return;
        }
        if (key == "commission_type") {
            if (value == "percent" || value == "strategy.commission.percent" || value == "0")
                commission_type_ = CommissionType::PERCENT;
            else if (value == "cash_per_order" || value == "strategy.commission.cash_per_order" || value == "1")
                commission_type_ = CommissionType::CASH_PER_ORDER;
            else if (value == "cash_per_contract" || value == "strategy.commission.cash_per_contract" || value == "2")
                commission_type_ = CommissionType::CASH_PER_CONTRACT;
            return;
        }
    }

    void on_bar(const Bar& bar) override {
        if (!inputs_initialized_) {
            alpha_       = get_input_double("Learning Rate", 0.10);
            gamma_       = get_input_double("Discount Factor", 0.95);
            epsilon_     = get_input_double("Epsilon Start", 0.20);
            eps_min_     = get_input_double("Epsilon Min", 0.02);
            eps_decay_   = get_input_double("Epsilon Decay", 0.999);
            switch_cost_ = get_input_double("Switch Cost Pct", 0.04);
            mom_thresh_  = get_input_double("Momentum Threshold", 0.001);
            warmup_bars_ = get_input_int("Warmup Bars", 100);
            int rsi_len      = get_input_int("RSI Length", 14);
            int ema_fast_len = get_input_int("EMA Fast Length", 10);
            int ema_slow_len = get_input_int("EMA Slow Length", 40);
            int atr_len      = get_input_int("ATR Length", 14);
            int atr_base_len = get_input_int("ATR Baseline Length", 96);
            rsi_          = ta::RSI(rsi_len);
            ema_fast_     = ta::EMA(ema_fast_len);
            ema_slow_     = ta::EMA(ema_slow_len);
            atr_          = ta::ATR(atr_len);
            atr_baseline_ = ta::SMA(atr_base_len);
            rng_ = rlq::Rng((uint64_t)get_input_int64("Seed", 20240607));
            inputs_initialized_ = true;
        }

        const double close = current_bar_.close;
        double rsi      = is_first_tick_ ? rsi_.compute(close) : rsi_.recompute(close);
        double ema_f    = is_first_tick_ ? ema_fast_.compute(close) : ema_fast_.recompute(close);
        double ema_s    = is_first_tick_ ? ema_slow_.compute(close) : ema_slow_.recompute(close);
        double atr      = is_first_tick_
            ? atr_.compute(current_bar_.high, current_bar_.low, close)
            : atr_.recompute(current_bar_.high, current_bar_.low, close);
        double atr_pct  = (close > 0.0 && !std::isnan(atr)) ? atr / close : na<double>();
        double atr_base = std::isnan(atr_pct)
            ? na<double>()
            : (is_first_tick_ ? atr_baseline_.compute(atr_pct) : atr_baseline_.recompute(atr_pct));

        // Learn + decide once per script bar. Without bar magnifier the engine
        // dispatches exactly one tick per aggregated bar, so this is the whole
        // path; the guard keeps the Q-update single-shot if a finer dispatch
        // mode is ever used.
        if (!is_first_tick_) return;

        double log_ret = (prev_close_ > 0.0 && close > 0.0)
            ? std::log(close / prev_close_) : 0.0;
        prev_close_ = close;
        bars_seen_++;

        bool features_ready = bars_seen_ > warmup_bars_
            && !std::isnan(rsi) && !std::isnan(ema_f) && !std::isnan(ema_s)
            && !std::isnan(atr_pct) && !std::isnan(atr_base);
        if (!features_ready) return;

        int state = encode_state(rsi, ema_f, ema_s, log_ret, atr_pct, atr_base);

        if (have_prev_) {
            // Reward in percent units: direction actually held over the bar
            // (the engine fills entries on the next bar open, so reading the
            // live position — not the requested action — keeps reward and
            // realised PnL aligned), minus the switch penalty booked when the
            // previous decision changed the target.
            double held = signed_position_size();
            double dir = (held > 0.0) ? 1.0 : (held < 0.0 ? -1.0 : 0.0);
            double reward = dir * log_ret * 100.0 - pending_cost_;
            pending_cost_ = 0.0;

            double best_next = *std::max_element(q_[state].begin(), q_[state].end());
            double& qsa = q_[prev_state_][prev_action_];
            qsa += alpha_ * (reward + gamma_ * best_next - qsa);
        }

        int action;
        if (rng_.uniform() < epsilon_) {
            action = rng_.uniform_int(kNumActions);
        } else {
            const auto& row = q_[state];
            action = (int)(std::max_element(row.begin(), row.end()) - row.begin());
        }
        epsilon_ = std::max(eps_min_, epsilon_ * eps_decay_);

        if (have_prev_ && action != prev_action_) pending_cost_ = switch_cost_;
        apply_action(action);

        prev_state_  = state;
        prev_action_ = action;
        have_prev_   = true;
    }

private:
    int encode_state(double rsi, double ema_f, double ema_s,
                     double log_ret, double atr_pct, double atr_base) const {
        int rsi_b = rsi < 30.0 ? 0 : rsi < 45.0 ? 1 : rsi < 55.0 ? 2 : rsi < 70.0 ? 3 : 4;
        int trend_b = (ema_f > ema_s) ? 1 : 0;
        int mom_b = log_ret < -mom_thresh_ ? 0 : (log_ret > mom_thresh_ ? 2 : 1);
        int vol_b = (atr_pct > atr_base) ? 1 : 0;
        return ((rsi_b * 2 + trend_b) * 3 + mom_b) * 2 + vol_b;
    }

    void apply_action(int action) {
        double pos = signed_position_size();
        if (action == kActLong) {
            if (pos <= 0.0) strategy_entry("RL-Long", true);
        } else if (action == kActShort) {
            if (pos >= 0.0) strategy_entry("RL-Short", false);
        } else {
            if (pos != 0.0) strategy_close_all();
        }
    }

    // Indicators (feature extractors).
    ta::RSI rsi_;
    ta::EMA ema_fast_;
    ta::EMA ema_slow_;
    ta::ATR atr_;
    ta::SMA atr_baseline_;  // baseline of ATR% — splits calm/volatile regimes

    // Q-learning state.
    std::array<std::array<double, kNumActions>, kNumStates> q_;
    rlq::Rng rng_;
    double alpha_ = 0.10, gamma_ = 0.95;
    double epsilon_ = 0.20, eps_min_ = 0.02, eps_decay_ = 0.999;
    double switch_cost_ = 0.04;   // % reward penalty per position change
    double mom_thresh_ = 0.001;   // log-return bucket edge (0.1% per bar)
    int warmup_bars_ = 100;

    int prev_state_ = 0;
    int prev_action_ = kActFlat;
    bool have_prev_ = false;
    double pending_cost_ = 0.0;
    double prev_close_ = na<double>();
    int bars_seen_ = 0;
    bool inputs_initialized_ = false;
};

extern "C" {
    void* strategy_create(const char* params_json) {
        (void)params_json;
        return new RLQLearnStrategy();
    }
    void run_backtest(void* s, Bar* bars, int n, ReportC* out) {
        auto* strat = static_cast<RLQLearnStrategy*>(s);
        strat->run(bars, n);
        strat->fill_report(out);
    }
    void run_backtest_full(void* s, Bar* bars, int n,
                           const char* input_tf, const char* script_tf,
                           int bar_magnifier, int magnifier_samples,
                           int magnifier_dist,
                           ReportC* out) {
        auto* strat = static_cast<RLQLearnStrategy*>(s);
        std::string itf = input_tf ? input_tf : "";
        std::string stf = script_tf ? script_tf : "";
        bool needs_full_run = (bar_magnifier != 0)
            || (!itf.empty() && !stf.empty() && itf != stf);
        if (!needs_full_run) {
            strat->run(bars, n);
        } else {
            strat->run(bars, n, itf, stf, bar_magnifier != 0, magnifier_samples,
                       static_cast<MagnifierDistribution>(magnifier_dist));
        }
        strat->fill_report(out);
    }
    void strategy_free(void* s) {
        delete static_cast<RLQLearnStrategy*>(s);
    }
    void report_free(ReportC* report) {
        BacktestEngine::free_report(report);
    }
    void strategy_set_input(void* s, const char* key, const char* value) {
        if (!s || !key || !value) return;
        static_cast<RLQLearnStrategy*>(s)->set_input(key, value);
    }
    void strategy_set_override(void* s, const char* key, const char* value) {
        if (!s || !key || !value) return;
        static_cast<RLQLearnStrategy*>(s)->set_strategy_override(key, value);
    }
    void strategy_set_magnifier_volume_weighted(void* s, int on) {
        if (!s) return;
        static_cast<RLQLearnStrategy*>(s)->set_magnifier_volume_weighted(on != 0);
    }
}
