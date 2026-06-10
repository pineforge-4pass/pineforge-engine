// strategies/rl-qlearn/strategy.cpp — tabular Q-learning trading agent
// built WITH the PineForge engine (hand-written C++, not Pine-transpiled).
//
// The agent trades ETH-USDT on 15-minute bars (the engine aggregates the
// 1-minute corpus feed). It supports two modes driven by the harness:
//
//   training  — epsilon-greedy exploration + Q-updates; the harness replays
//               the training slice for many epochs, persisting the Q-table
//               between passes via strategy_save_qtable/strategy_load_qtable.
//   greedy    — "Greedy Mode" input: epsilon = 0, learning rate = 0; the
//               frozen policy is evaluated (out-of-sample when run on bars
//               the agent never trained on).
//
// Agent design
// ------------
//   State (108): RSI(14) bucket [3] x EMA(40)>EMA(160) trend [2] x 96-bar
//          (24h) momentum [3] x ATR%-vs-SMA(96) volatility regime [2]
//          x CURRENT POSITION [3].
//          Two hard-won design points:
//            - SLOW market features: on 15m ETH bars the persistent edge net
//              of taker fees sits at the 10h/40h trend horizon — fast
//              features (1-bar momentum, EMA 10/40) flip state every few
//              bars and churn away the edge in commission.
//            - The position MUST be part of the state. Without it the
//              bootstrap term max_a' Q(s',a') is identical for every action,
//              so the action choice degenerates to a one-step reward
//              comparison: a +0.002%/bar conditional edge can never beat a
//              0.1% entry cost and "always flat" becomes a self-consistent
//              fixed point. With the position in the state, the value of
//              BEING long in an uptrend accumulates across the regime
//              (mean trend run ~121 bars) and amortises the entry cost.
//   Action (3): FLAT, LONG, SHORT — strategy_close_all()/strategy_entry();
//          a reversal is a single entry (TV semantics).
//   Reward: direction held during the elapsed bar x bar log-return (in %),
//          minus a switch penalty proportional to the position change the
//          previous decision caused (1 unit to enter or exit, 2 units to
//          reverse) at the per-fill commission rate, so the agent
//          internalises trading costs without double-charging round trips.
//   Update: Q(s,a) += alpha * (r + gamma * max_a' Q(s',a') - Q(s,a)),
//          epsilon-greedy, seeded xorshift64* PRNG — bit-reproducible.

#include <pineforge/engine.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
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

    // 3 RSI x 2 trend x 3 momentum(96-bar) x 2 volatility x 3 position.
    static constexpr int kNumStates = 3 * 2 * 3 * 2 * 3;

    explicit RLQLearnStrategy()
        : rsi_(14), ema_fast_(40), ema_slow_(160), atr_(14), atr_baseline_(96),
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

    // --- Q-table persistence (harness replays epochs across runs) -------
    bool save_qtable(const char* path) const {
        std::FILE* f = std::fopen(path, "w");
        if (!f) return false;
        for (int s = 0; s < kNumStates; ++s)
            std::fprintf(f, "%.17g %.17g %.17g\n", q_[s][0], q_[s][1], q_[s][2]);
        std::fclose(f);
        return true;
    }

    bool load_qtable(const char* path) {
        std::FILE* f = std::fopen(path, "r");
        if (!f) return false;
        for (int s = 0; s < kNumStates; ++s) {
            if (std::fscanf(f, "%lf %lf %lf", &q_[s][0], &q_[s][1], &q_[s][2]) != 3) {
                std::fclose(f);
                return false;
            }
        }
        std::fclose(f);
        return true;
    }

    void on_bar(const Bar& bar) override {
        if (!inputs_initialized_) {
            alpha_        = get_input_double("Learning Rate", 0.10);
            gamma_        = get_input_double("Discount Factor", 0.998);
            epsilon_      = get_input_double("Epsilon Start", 0.20);
            eps_min_      = get_input_double("Epsilon Min", 0.02);
            eps_decay_    = get_input_double("Epsilon Decay", 0.999);
            switch_cost_  = get_input_double("Switch Cost Pct", 0.05);
            mom_lookback_ = get_input_int("Momentum Lookback", 96);
            mom_thresh_   = get_input_double("Momentum Threshold", 0.02);
            warmup_bars_  = get_input_int("Warmup Bars", 200);
            if (get_input_bool("Greedy Mode", false)) {
                // Frozen-policy evaluation: no exploration, no learning.
                epsilon_ = 0.0;
                eps_min_ = 0.0;
                alpha_   = 0.0;
            }
            int rsi_len      = get_input_int("RSI Length", 14);
            int ema_fast_len = get_input_int("EMA Fast Length", 40);
            int ema_slow_len = get_input_int("EMA Slow Length", 160);
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

        close_hist_.push_back(close);
        double mom = na<double>();
        if ((int)close_hist_.size() > mom_lookback_) {
            double past = close_hist_.front();
            close_hist_.pop_front();
            if (past > 0.0) mom = std::log(close / past);
        }

        bool features_ready = bars_seen_ > warmup_bars_
            && !std::isnan(rsi) && !std::isnan(ema_f) && !std::isnan(ema_s)
            && !std::isnan(atr_pct) && !std::isnan(atr_base) && !std::isnan(mom);
        if (!features_ready) return;

        // Position actually held over the elapsed bar (entries fill on the
        // bar open, so by on_bar time this reflects the previous decision).
        double held = signed_position_size();
        int pos_b = (held > 0.0) ? 1 : (held < 0.0 ? 2 : 0);
        int state = encode_state(rsi, ema_f, ema_s, mom, atr_pct, atr_base, pos_b);

        if (have_prev_) {
            // Reward in percent units: direction actually held over the bar
            // (the engine fills entries on the next bar open, so reading the
            // live position — not the requested action — keeps reward and
            // realised PnL aligned), minus the switch penalty booked when the
            // previous decision changed the target.
            double dir = (held > 0.0) ? 1.0 : (held < 0.0 ? -1.0 : 0.0);
            double reward = dir * log_ret * 100.0 - pending_cost_;
            pending_cost_ = 0.0;

            double best_next = *std::max_element(q_[state].begin(), q_[state].end());
            double& qsa = q_[prev_state_][prev_action_];
            qsa += alpha_ * (reward + gamma_ * best_next - qsa);
        }

        int action;
        if (epsilon_ > 0.0 && rng_.uniform() < epsilon_) {
            action = rng_.uniform_int(kNumActions);
        } else {
            const auto& row = q_[state];
            action = (int)(std::max_element(row.begin(), row.end()) - row.begin());
        }
        epsilon_ = std::max(eps_min_, epsilon_ * eps_decay_);

        if (have_prev_) {
            // Cost in units of position changed: FLAT<->LONG/SHORT moves 1
            // unit, LONG<->SHORT reverses 2. switch_cost_ is the per-fill
            // commission in %, so a round trip costs 2 x switch_cost_ total.
            static constexpr int kTarget[kNumActions] = {0, 1, -1};
            int units = std::abs(kTarget[action] - kTarget[prev_action_]);
            pending_cost_ = switch_cost_ * units;
        }
        apply_action(action);

        prev_state_  = state;
        prev_action_ = action;
        have_prev_   = true;
    }

private:
    int encode_state(double rsi, double ema_f, double ema_s,
                     double mom, double atr_pct, double atr_base,
                     int pos_b) const {
        int rsi_b = rsi < 40.0 ? 0 : rsi < 60.0 ? 1 : 2;
        int trend_b = (ema_f > ema_s) ? 1 : 0;
        int mom_b = mom < -mom_thresh_ ? 0 : (mom > mom_thresh_ ? 2 : 1);
        int vol_b = (atr_pct > atr_base) ? 1 : 0;
        return (((rsi_b * 2 + trend_b) * 3 + mom_b) * 2 + vol_b) * 3 + pos_b;
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
    std::deque<double> close_hist_;  // last N closes for the momentum bucket

    // Q-learning state.
    std::array<std::array<double, kNumActions>, kNumStates> q_;
    rlq::Rng rng_;
    double alpha_ = 0.10, gamma_ = 0.998;
    double epsilon_ = 0.20, eps_min_ = 0.02, eps_decay_ = 0.999;
    double switch_cost_ = 0.05;    // % reward penalty per unit of position change
    int mom_lookback_ = 96;        // momentum horizon in bars (96 x 15m = 24h)
    double mom_thresh_ = 0.02;     // momentum log-return bucket edge (2%)
    int warmup_bars_ = 200;

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
    int strategy_save_qtable(void* s, const char* path) {
        if (!s || !path) return 0;
        return static_cast<RLQLearnStrategy*>(s)->save_qtable(path) ? 1 : 0;
    }
    int strategy_load_qtable(void* s, const char* path) {
        if (!s || !path) return 0;
        return static_cast<RLQLearnStrategy*>(s)->load_qtable(path) ? 1 : 0;
    }
}
