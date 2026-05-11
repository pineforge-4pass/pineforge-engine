#pragma once
#include <vector>
#include <string>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include "na.hpp"
#include "bar.hpp"
#include "timeframe.hpp"
#include "magnifier.hpp"

namespace pineforge {

enum class PositionSide { FLAT, LONG, SHORT };

// Forward declaration of an internal enum used by some BacktestEngine
// method signatures. The full definition lives in src/engine_internal.hpp
// (private to libruntime); only the underlying-type pin is needed here.
namespace internal {
enum class DualEntryStopPathWinner : int;
}

struct PyramidEntry {
    double price;
    int64_t time;
    double qty;
    std::string entry_id;
    int entry_bar_index = -1;
    std::string entry_comment;
    double max_runup = 0.0;
    double max_drawdown = 0.0;
};

struct Trade {
    int64_t entry_time;
    int64_t exit_time;
    double entry_price;
    double exit_price;
    double qty;
    double pnl;
    double pnl_pct;
    bool is_long;
    int entry_bar_index = -1;
    int exit_bar_index = -1;
    std::string entry_id;
    std::string entry_comment;
    std::string exit_comment;
    std::string exit_id;
    double max_runup = 0.0;
    double max_drawdown = 0.0;
};

struct TradeC {
    int64_t entry_time;
    int64_t exit_time;
    double entry_price;
    double exit_price;
    double pnl;
    double pnl_pct;
    int is_long;
    // Max Adverse/Favorable Excursion expressed as $ move per unit qty.
    // max_runup is peak favorable move (price travel in direction of trade).
    // max_drawdown is peak adverse move (price travel against trade).
    double max_runup;
    double max_drawdown;
    double qty;
};

struct SecurityDiagC {
    int sec_id;
    int64_t feed_count;
    int64_t eval_complete_count;
    int64_t eval_partial_count;
};

// Per-bar runtime trace entry. The transpiler's ``trace`` pragma emits
// ``trace(name, value)`` calls inside the generated ``on_bar`` so the
// validator can replay engine-internal series alongside TradingView's own
// per-bar plot data and pinpoint the bar/filter where divergence appears.
//
// ``name_id`` indexes into ``ReportC::trace_names`` — names are interned
// once per unique label so the per-call cost is push_back of a 24-byte POD
// rather than a string copy.
struct TraceEntryC {
    int64_t timestamp;
    int32_t bar_index;
    int32_t name_id;
    double value;
};

struct ReportC {
    int total_trades;
    TradeC* trades;
    int trades_len;
    double net_profit;
    int64_t input_bars_processed;
    int64_t script_bars_processed;
    int64_t security_feeds_total;
    int64_t security_eval_complete_total;
    int64_t security_eval_partial_total;
    int64_t magnifier_sub_bars_total;
    int64_t magnifier_sample_ticks_total;
    int input_tf_seconds;
    int script_tf_seconds;
    int script_tf_ratio;
    int needs_aggregation;
    int bar_magnifier_enabled;
    SecurityDiagC* security_diag;
    int security_diag_len;
    // Per-bar trace records emitted by ``BacktestEngine::trace`` calls. Both
    // arrays are heap-allocated by ``fill_report`` and freed by
    // ``free_report``; both are nullptr / 0-length when tracing was disabled
    // or no calls were made. ``trace_names`` is a flat name table — each
    // ``TraceEntryC.name_id`` indexes into it. Pointers in ``trace_names``
    // are stable C-strings owned by the live ``BacktestEngine`` instance
    // (its ``trace_names_`` vector); they remain valid until ``strategy_free``.
    TraceEntryC* trace;
    int trace_len;
    const char** trace_names;
    int trace_names_len;
};

enum class OrderType { MARKET, ENTRY, EXIT, RAW_ORDER };

struct PendingOrder {
    std::string id;
    std::string from_entry;    // for exit orders
    OrderType type;
    bool is_long;
    double limit_price;        // NaN = not set
    double stop_price;         // NaN = not set
    double trail_points;       // NaN = not set
    double trail_offset;       // NaN = not set
    double qty;                // NaN = use default sizing, else explicit qty
    int qty_type;              // -1 = qty is fixed contracts, else QtyType override
    double qty_percent;        // 100 = full position
    std::string oca_name;      // OCA group name
    int oca_type;              // 0=none, 1=cancel, 2=reduce
    int created_bar;           // bar_index when order was created
    int64_t created_seq = 0;
    PositionSide created_position_side = PositionSide::FLAT;
    // Snapshot of the position's quantity at the moment this order was
    // PLACED (0 if placed from flat). Used by execute_market_entry's
    // flat branch to apply TradingView's deferred-flip growth rule:
    // when a priced entry was placed against a position that was later
    // closed (by strategy.close, close_all, or any other path) and the
    // entry now fires from flat in the opposite direction, TV opens the
    // new position at ``qty + tv_carry_qty`` rather than ``qty``.
    // Verified empirically with probe 92's 20 deferred flips that fire
    // after the daily ``strategy.close_all`` cleanup, hours after the
    // closing bar — so this MUST persist across bars rather than being
    // a per-bar transient state.
    //
    // ``strategy.entry`` with the same id REPLACES the pending order
    // entirely — including a fresh ``tv_carry_qty = position_qty_``
    // snapshot. This is critical for probes 52 / 63 / 95 / 96 where
    // SE/LE is re-placed every bar a cross condition holds: once
    // ``strategy.close`` flushes the position to 0, subsequent
    // re-placements capture carry=0, so when the priced entry finally
    // fires the chain resets to qty=1 (matching TV's behaviour at
    // probe 52 trade 113). Preserving the largest observed carry
    // across re-placements would over-extend chains.
    double tv_carry_qty = 0.0;
    std::string comment;       // order comment for trade reporting
    bool requested_partial = false;         // true iff caller passed qty_percent < 100
    bool created_while_in_position = false;  // true if position was open when order was placed
};

// default_qty_type constants (matches TradingView)
enum class QtyType { FIXED = 0, PERCENT_OF_EQUITY = 1, CASH = 2 };

// commission_type constants
enum class CommissionType { PERCENT = 0, CASH_PER_ORDER = 1, CASH_PER_CONTRACT = 2 };

// Pine user enum → str.tostring (field payloads). Transpiler enforces enum decl before
// input.enum; this clamps the index so bad values never read past the table.
inline const std::string& pine_enum_str_at(const std::string* table, std::size_t n,
                                           int idx) {
    static const std::string kEmpty;
    if (n == 0 || table == nullptr) return kEmpty;
    std::size_t u = static_cast<std::size_t>(idx);
    if (u >= n) u = n - 1;
    return table[u];
}

struct SymInfo {
    std::string ticker = "UNKNOWN";
    std::string tickerid = "UNKNOWN";
    std::string currency = "USD";
    std::string basecurrency = "";
    std::string type = "crypto";
    std::string timezone = "UTC";
    std::string session = "24x7";
    std::string volumetype = "base";
    std::string description = "";
    double mintick = 0.01;
    double pointvalue = 1.0;
};

struct StrategyOverrides {
    double initial_capital = std::numeric_limits<double>::quiet_NaN();
    double commission_value = std::numeric_limits<double>::quiet_NaN();
    double default_qty_value = std::numeric_limits<double>::quiet_NaN();
    int pyramiding = -1;
    int slippage = -1;
    int commission_type = -1;
    int default_qty_type = -1;
    int process_orders_on_close = -1;
    int close_entries_rule = -1;
};

class BacktestEngine {
protected:
    // --- Position state ---
    PositionSide position_side_ = PositionSide::FLAT;
    double position_entry_price_ = 0.0;   // volume-weighted average (for strategy calculations)
    int64_t position_entry_time_ = 0;
    double position_qty_ = 1.0;
    int position_entry_count_ = 0;  // number of entries in current direction (for pyramiding)
    int position_open_bar_ = -1;    // bar_index_ when position was opened (for exit delay)
    std::vector<PyramidEntry> pyramid_entries_;  // individual entries for trade reporting

    // --- Strategy parameters (set from strategy() declaration) ---
    double initial_capital_ = 1000000.0;
    bool process_orders_on_close_ = false;
    QtyType default_qty_type_ = QtyType::FIXED;
    double default_qty_value_ = 1.0;
    int pyramiding_ = 1;            // max additional entries in same direction
    CommissionType commission_type_ = CommissionType::PERCENT;
    double commission_value_ = 0.0;
    int slippage_ = 0;              // slippage in ticks
    double syminfo_mintick_ = 0.01; // tick size for slippage calculation
    int max_intraday_filled_orders_ = 0; // 0 = unlimited
    bool close_entries_rule_any_ = false; // true = "ANY", false = "FIFO" (default)
    // Percentage of margin required to open a long/short position. Default
    // 100 = 1x leverage (no leverage). TradingView's strategy() takes these
    // as ``margin_long`` / ``margin_short``; when the implied position value
    // (qty * fill_price * margin_pct / 100) exceeds the strategy's available
    // equity, TV silently rejects the fill — the entry simply does not appear
    // in the trade list. The PineForge engine mirrors that rejection in
    // execute_market_entry's FLAT and pyramid-add branches; without it, a
    // dynamic-qty strategy like community/IES, community/VCP, or
    // ies-probe-08 over-leverages on low-ATR bars and produces ~5x more
    // trades than TV. Validated by the matched-trade qty ratio in probe 08
    // exactly equalling engine_equity / TV_equity.
    double margin_long_ = 100.0;
    double margin_short_ = 100.0;

    // True iff the user's strategy.pine contains at least one
    // ``strategy.close`` or ``strategy.close_all`` call (compile-time
    // determined by the codegen, set in the generated class's
    // constructor). Gates the TradingView deferred-flip growth rule
    // applied in ``execute_market_entry``'s FLAT branch.
    bool script_has_strategy_close_ = false;
    int64_t trade_start_time_ = std::numeric_limits<int64_t>::min();

    // Cumulative qty of ``strategy.close`` / ``strategy.close_all`` calls
    // issued during the CURRENT on_bar. Reset at the start of every bar
    // before strategy logic runs. Subtracted from ``position_qty_`` when
    // computing ``tv_carry_qty`` for a subsequent ``strategy.entry`` in
    // the same on_bar — TradingView evaluates calls in source order, so
    // a ``strategy.close`` call ahead of a ``strategy.entry`` in the
    // same block makes the entry capture the POST-CLOSE position size
    // for its carry. Verified by probe 93 cycle B: when the strategy
    // calls ``strategy.close("L2")`` before ``strategy.entry("S2",
    // stop=...)``, TV's S2 fires from flat at qty=1 (no growth);
    // cycle A reverses the order and the entry captures the still-open
    // position size, firing later with qty=2 (growth).
    double pending_close_qty_in_bar_ = 0.0;


    // --- SymInfo + Input injection ---
    SymInfo syminfo_;
    std::unordered_map<std::string, std::string> inputs_;

    // Input injection helpers for generated code
    double get_input_double(const std::string& key, double default_val) const;
    int get_input_int(const std::string& key, int default_val) const;
    bool get_input_bool(const std::string& key, bool default_val) const;
    std::string get_input_string(const std::string& key, const std::string& default_val) const;

    // --- Runtime state ---
    Bar current_bar_;
    int bar_index_ = 0;
    int64_t next_order_seq_ = 1;
    // TV: at most one priced ENTRY "open" event per bar; persists across
    // multiple process_pending_orders calls (bar magnifier) and dual-pass
    // opposing-stop resolution (see engine.cpp).
    int priced_entry_activity_bar_ = -1;
    bool priced_entry_filled_this_bar_ = false;

    std::vector<Trade> trades_;
    std::vector<PendingOrder> pending_orders_;

    // strategy.exit partial orders are one-shot per open position for a given id
    std::unordered_set<std::string> consumed_partial_exit_ids_;

    // --- Trailing stop state ---
    // Best favorable price since position entry (for trailing stop computation)
    double trail_best_price_ = std::numeric_limits<double>::quiet_NaN();

    // --- Intraday fill counter ---
    int intraday_fill_count_ = 0;
    int intraday_day_ = -1;  // day of year for reset

    // --- Cached trade metrics (updated incrementally in execute_market_exit) ---
    double net_profit_sum_ = 0.0;
    double gross_profit_sum_ = 0.0;
    double gross_loss_sum_ = 0.0;
    int win_trades_count_ = 0;
    int loss_trades_count_ = 0;

    // --- Equity extremes for max_drawdown / max_runup ---
    double max_equity_ = 0.0;    // peak equity for drawdown
    double max_drawdown_ = 0.0;  // maximum drawdown (positive number)
    double max_runup_ = 0.0;     // maximum runup (positive number)
    double min_equity_ = 0.0;    // trough equity for runup

    // --- Risk management (strategy.risk.*) ---
    enum class RiskDirection { BOTH, LONG_ONLY, SHORT_ONLY };
    RiskDirection risk_direction_ = RiskDirection::BOTH;
    int risk_max_cons_loss_days_ = 0;       // 0 = unlimited
    double risk_max_drawdown_ = 0.0;        // 0 = unlimited
    bool risk_max_drawdown_is_pct_ = false; // true = percent_of_equity mode
    double risk_max_intraday_loss_ = 0.0;   // 0 = unlimited
    bool risk_max_intraday_loss_is_pct_ = false; // true = percent_of_equity mode
    double risk_max_position_size_ = 0.0;   // 0 = unlimited

    // Risk state tracking
    int cons_loss_day_count_ = 0;
    int last_loss_day_ = -1;
    bool risk_halted_ = false;
    double intraday_pnl_ = 0.0;
    int intraday_pnl_day_ = -1;

    bool check_risk_allow_entry(bool is_long) const;
    void update_risk_state();

    // --- Per-trade extreme tracking ---
    void update_per_trade_extremes();

    // --- Strategy order commands ---
    // NOTE: prior to v0.2 the runtime accepted a leading `double market_price`
    // positional after `is_long`. The implementation never read it; every
    // fill price came from `current_bar_.close` inside the function body,
    // and every closed-transpiler call site passed `current_bar_.close`
    // verbatim. Parameter dropped to match TradingView's `strategy.entry()`
    // surface. Consumer codegen must be regenerated alongside this commit.
    void strategy_entry(const std::string& id, bool is_long,
                        double limit_price = std::numeric_limits<double>::quiet_NaN(),
                        double stop_price = std::numeric_limits<double>::quiet_NaN(),
                        double qty = std::numeric_limits<double>::quiet_NaN(),
                        const std::string& comment = "",
                        const std::string& oca_name = "",
                        int oca_type = 0,
                        int qty_type = -1);
    void strategy_close(const std::string& id, const std::string& comment = "",
                        double qty = std::numeric_limits<double>::quiet_NaN(),
                        double qty_percent = std::numeric_limits<double>::quiet_NaN(),
                        bool immediately = false);
    void strategy_close_all();
    void strategy_exit(const std::string& id, const std::string& from_entry,
                       double limit_price, double stop_price,
                       double trail_points = std::numeric_limits<double>::quiet_NaN(),
                       double trail_offset = std::numeric_limits<double>::quiet_NaN(),
                       double trail_price = std::numeric_limits<double>::quiet_NaN(),
                       double qty_percent = 100.0,
                       const std::string& comment = "");
    void strategy_cancel(const std::string& id);
    void strategy_cancel_all();
    void strategy_order(const std::string& id, bool is_long, double qty,
                        double limit_price = std::numeric_limits<double>::quiet_NaN(),
                        double stop_price = std::numeric_limits<double>::quiet_NaN(),
                        const std::string& oca_name = "",
                        int oca_type = 0);

    void process_pending_orders(const Bar& bar);

    // --- Slippage helper ---
    double round_to_mintick(double price) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        return std::round(price / syminfo_mintick_) * syminfo_mintick_;
    }

    // TradingView fills stop entries directionally to mintick rather than
    // rounding to nearest: long stops snap UP (ceil), short stops snap DOWN
    // (floor). Verified against basic/parabolic-asr where the 2,513
    // non-gap stop entry fills show a perfectly one-sided +/-0.01 bias.
    // See investigation report at /tmp/pf_investigation_parabolic_asr.md.
    double round_to_mintick_directional(double price, bool is_long_stop) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        double r = price / syminfo_mintick_;
        return (is_long_stop ? std::ceil(r) : std::floor(r)) * syminfo_mintick_;
    }

    double apply_slippage(double price, bool is_buy) const {
        if (slippage_ == 0) return round_to_mintick(price);
        double slip = slippage_ * syminfo_mintick_;
        return round_to_mintick(is_buy ? price + slip : price - slip);
    }

    // --- Commission helper ---
    double calc_commission(double fill_price, double qty) const {
        switch (commission_type_) {
            case CommissionType::PERCENT:
                return fill_price * qty * (commission_value_ / 100.0);
            case CommissionType::CASH_PER_ORDER:
                return commission_value_;
            case CommissionType::CASH_PER_CONTRACT:
                return commission_value_ * qty;
        }
        return 0.0;
    }

    // --- Position sizing helper ---
    double calc_qty(double fill_price) const {
        switch (default_qty_type_) {
            case QtyType::FIXED:
                return default_qty_value_;
            case QtyType::PERCENT_OF_EQUITY: {
                double equity = current_equity();
                double cash = equity * (default_qty_value_ / 100.0);
                return (fill_price > 0) ? (cash / fill_price) : default_qty_value_;
            }
            case QtyType::CASH:
                return (fill_price > 0) ? (default_qty_value_ / fill_price) : default_qty_value_;
        }
        return default_qty_value_;
    }

    // --- Strategy variable accessors ---
    double signed_position_size() const {
        if (position_side_ == PositionSide::LONG) return position_qty_;
        if (position_side_ == PositionSide::SHORT) return -position_qty_;
        return 0.0;
    }

    double net_profit() const { return net_profit_sum_; }
    double gross_profit() const { return gross_profit_sum_; }
    double gross_loss() const { return gross_loss_sum_; }
    double current_equity() const { return initial_capital_ + net_profit_sum_; }

    double max_runup_percent() const {
        return (initial_capital_ > 0.0) ? (max_runup_ / initial_capital_) * 100.0 : 0.0;
    }
    double grossprofit_percent() const {
        return (initial_capital_ > 0.0) ? (gross_profit_sum_ / initial_capital_) * 100.0 : 0.0;
    }
    double grossloss_percent() const {
        return (initial_capital_ > 0.0) ? (gross_loss_sum_ / initial_capital_) * 100.0 : 0.0;
    }
    double avg_trade() const {
        int n = (int)trades_.size();
        return (n > 0) ? (net_profit_sum_ / (double)n) : 0.0;
    }
    double avg_trade_percent() const {
        int n = (int)trades_.size();
        if (n <= 0) return 0.0;
        double s = 0.0;
        for (const auto& t : trades_) s += t.pnl_pct;
        return s / (double)n;
    }
    double avg_winning_trade() const {
        return (win_trades_count_ > 0) ? (gross_profit_sum_ / (double)win_trades_count_) : 0.0;
    }
    double avg_losing_trade() const {
        return (loss_trades_count_ > 0) ? (gross_loss_sum_ / (double)loss_trades_count_) : 0.0;
    }
    double avg_winning_trade_percent() const {
        if (win_trades_count_ <= 0) return 0.0;
        double s = 0.0;
        int c = 0;
        for (const auto& t : trades_) {
            if (t.pnl > 0.0) { s += t.pnl_pct; ++c; }
        }
        return (c > 0) ? (s / (double)c) : 0.0;
    }
    double avg_losing_trade_percent() const {
        if (loss_trades_count_ <= 0) return 0.0;
        double s = 0.0;
        int c = 0;
        for (const auto& t : trades_) {
            if (t.pnl < 0.0) { s += t.pnl_pct; ++c; }
        }
        return (c > 0) ? (s / (double)c) : 0.0;
    }
    double margin_liquidation_price() const { return na<double>(); }
    double open_trades_capital_held() const {
        if (position_side_ == PositionSide::FLAT) return 0.0;
        return std::abs(position_qty_ * position_entry_price_);
    }

    double open_profit(double current_price) const {
        if (position_side_ == PositionSide::FLAT) return 0.0;
        double diff = (position_side_ == PositionSide::LONG)
            ? (current_price - position_entry_price_)
            : (position_entry_price_ - current_price);
        return diff * position_qty_;
    }

    int count_wintrades() const { return win_trades_count_; }
    int count_losstrades() const { return loss_trades_count_; }

    // --- Time/date extraction from bar timestamp (UTC) ---
    struct BarTime {
        int year, month, dayofmonth, hour, minute, second, dayofweek, weekofyear;
    };

    BarTime _decompose_bar_time() const {
        time_t secs = (time_t)(current_bar_.timestamp / 1000);
        struct tm tm_buf;
        gmtime_r(&secs, &tm_buf);
        BarTime bt;
        bt.year = tm_buf.tm_year + 1900;
        bt.month = tm_buf.tm_mon + 1;
        bt.dayofmonth = tm_buf.tm_mday;
        bt.hour = tm_buf.tm_hour;
        bt.minute = tm_buf.tm_min;
        bt.second = tm_buf.tm_sec;
        bt.dayofweek = tm_buf.tm_wday + 1;
        bt.weekofyear = (tm_buf.tm_yday + 7 - ((tm_buf.tm_wday + 6) % 7)) / 7;
        return bt;
    }

    int _bar_hour() const { return _decompose_bar_time().hour; }
    int _bar_minute() const { return _decompose_bar_time().minute; }
    int _bar_second() const { return _decompose_bar_time().second; }
    int _bar_dayofmonth() const { return _decompose_bar_time().dayofmonth; }
    int _bar_dayofweek() const { return _decompose_bar_time().dayofweek; }
    int _bar_month() const { return _decompose_bar_time().month; }
    int _bar_year() const { return _decompose_bar_time().year; }
    int _bar_weekofyear() const { return _decompose_bar_time().weekofyear; }

    // --- Bar magnifier state ---
    bool bar_magnifier_enabled_ = false;
    bool is_first_tick_ = true;
    bool is_last_tick_ = true;
    bool barstate_islast_ = false;
    int magnifier_samples_ = 4;
    MagnifierDistribution magnifier_dist_ = MagnifierDistribution::ENDPOINTS;
    // When true, run_magnified_bar scales per-sub-bar sample count by
    // (sub_bar.volume / mean_sub_bar_volume) within each script bar — dense
    // tick approximation on high-volume sub-bars without real tick data.
    bool magnifier_volume_weighted_ = false;

    // --- Timeframe state ---
    std::string input_tf_;
    std::string script_tf_;
    TimeframeAggregator script_tf_agg_;
    int64_t prev_bar_timestamp_ = 0;

    // --- request.security state ---
    struct SecurityEvalState {
        int sec_id = 0;
        std::string tf;
        TimeframeAggregator aggregator;
        Bar current_bar{};
        bool gaps_on = false;
        bool lookahead_on = false;
        bool lower_tf_requested = false;
        bool lower_tf_emulation = false;
        int lower_tf_ratio = 0;
        int lower_tf_seconds = 0;
        int current_sub_bar_count = 0;
        int64_t feed_count = 0;
        int64_t eval_complete_count = 0;
        int64_t eval_partial_count = 0;
        // ``request.security_lower_tf`` returns one element per
        // synthesised sub-bar of the current chart bar, so the codegen
        // needs to know which sub-bar inside the current chart bar is
        // currently being processed by the per-sec_id evaluator method.
        // ``lower_tf_array_requested`` is set by
        // ``register_security_lower_tf_eval`` and forces an extra
        // lower-TF-emulation validity check in
        // ``validate_security_timeframes``. ``lower_tf_sub_bar_index``
        // is reset to 0 at the start of every
        // ``feed_security_eval_state`` invocation in lower-TF
        // emulation mode and incremented after each per-sub-bar
        // dispatch so the codegen can clear its accumulator on index
        // 0 and then push for every subsequent sub-bar.
        bool lower_tf_array_requested = false;
        int lower_tf_sub_bar_index = 0;
        // ``lower_tf_use_input`` selects the input-passthrough LTF path:
        // when the requested TF is >= input_tf and < script_tf we hand
        // the per-script-bar window of real input bars to the codegen
        // (optionally roll-up aggregated when req > input). Mutually
        // exclusive with ``lower_tf_emulation`` (synthesis) — only one
        // is set per state. ``lower_tf_input_aggregation_ratio`` is
        // ``req_seconds / input_seconds`` (>=1; 1 means raw passthrough,
        // N means N raw input bars roll up into one returned LTF bar).
        // ``lower_tf_input_buffer`` accumulates raw input bars within
        // the current script-TF chunk and is flushed at chunk
        // completion (or at end of feed for trailing partial chunks).
        bool lower_tf_use_input = false;
        int lower_tf_input_aggregation_ratio = 1;
        std::vector<Bar> lower_tf_input_buffer;
    };

    std::vector<SecurityEvalState> security_eval_states_;

    // --- Runtime trace state ---
    // Gated by ``trace_enabled_`` (default false) so production strategies
    // pay zero cost; the validator flips it on per-strategy when it needs
    // engine-internal per-bar values for TV cross-referencing.
    //
    // ``trace_buffer_`` is a flat vector of POD records (no string allocs
    // per call); each entry holds a ``name_id`` indexing into
    // ``trace_names_``, which is the unique-name table built by
    // ``intern_trace_name``. A first-time name pays one ``unordered_map``
    // insert + ``string`` push_back; subsequent calls with the same name
    // are a single map lookup.
    bool trace_enabled_ = false;
    std::vector<TraceEntryC> trace_buffer_;
    std::vector<std::string> trace_names_;
    std::unordered_map<std::string, int32_t> trace_name_index_;

    int32_t intern_trace_name(const std::string& name);

    int64_t diag_input_bars_processed_ = 0;
    int64_t diag_script_bars_processed_ = 0;
    int64_t diag_magnifier_sub_bars_processed_ = 0;
    int64_t diag_magnifier_sample_ticks_processed_ = 0;
    int diag_script_tf_ratio_ = 0;
    bool diag_needs_aggregation_ = false;

    // Captured by the public run() wrappers when the underlying engine logic
    // throws. Cleared at the start of every run(). Surfaces through
    // last_error() / pf_strategy_get_last_error() so the C ABI never
    // unwinds a C++ exception across the extern "C" boundary.
    std::string last_error_;

    void register_security_eval(int sec_id, const std::string& requested_tf,
                                const std::string& input_tf, bool lookahead_on,
                                bool gaps_on = false);
    // ``request.security_lower_tf`` registers the same per-sec_id eval
    // state but with the additional contract that the requested TF must
    // resolve to a finer-than-input TF emulation. This wrapper sets the
    // ``lower_tf_array_requested`` flag so ``validate_security_timeframes``
    // can throw a precise error if the chart's input TF turns out to be
    // <= the requested TF (mirroring TradingView's "lower timeframe
    // required" error for ``request.security_lower_tf``).
    void register_security_lower_tf_eval(int sec_id, const std::string& requested_tf,
                                         const std::string& input_tf);
    // Sub-bar index (0-based) of the current ``request.security_lower_tf``
    // synthesis within the current chart bar. Returns 0 outside the
    // synthesis loop. Used by codegen to clear its per-call vector at
    // sub-bar 0 and push one element per sub-bar after.
    int security_lower_tf_sub_bar_index(int sec_id) const;
    void validate_security_timeframes(const std::string& input_tf);
    bool security_series_slot_is_new(int sec_id) const;
    void feed_security_eval_state(SecurityEvalState& state, const Bar& input_bar);

    virtual void configure_security_evaluators() {}
    virtual void evaluate_security(int sec_id, const Bar& bar, bool is_complete) {}
    virtual void clear_security(int sec_id) {}

    // Magnifier helpers
    void run_magnified_bar(const std::vector<Bar>& sub_bars);
    virtual void finalize_bar() {}

    // --- Equity extremes update (called after each on_bar) ---
    void update_equity_extremes() {
        double eq = initial_capital_ + net_profit_sum_ + open_profit(current_bar_.close);
        if (eq > max_equity_) {
            max_equity_ = eq;
            min_equity_ = eq;  // reset trough on new peak
        }
        if (eq < min_equity_) {
            min_equity_ = eq;
        }
        double dd = max_equity_ - eq;
        if (dd > max_drawdown_) max_drawdown_ = dd;
        double ru = eq - min_equity_;
        if (ru > max_runup_) max_runup_ = ru;
    }

    // --- Trade history accessors (for strategy.closedtrades.*) ---
    double closed_trade_profit(int index) const {
        if (index >= 0 && index < (int)trades_.size())
            return trades_[index].pnl;
        return 0.0;
    }
    double closed_trade_profit_percent(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::numeric_limits<double>::quiet_NaN();
        return trades_[idx].pnl_pct;
    }
    double closed_trade_commission(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::numeric_limits<double>::quiet_NaN();
        const Trade& t = trades_[idx];
        return calc_commission(t.entry_price, t.qty) + calc_commission(t.exit_price, t.qty);
    }
    int closed_trade_entry_bar_index(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return na<int>();
        return trades_[idx].entry_bar_index;
    }
    int closed_trade_exit_bar_index(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return na<int>();
        return trades_[idx].exit_bar_index;
    }
    std::string closed_trade_entry_comment(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::string();
        return trades_[idx].entry_comment;
    }
    std::string closed_trade_exit_comment(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::string();
        return trades_[idx].exit_comment;
    }
    std::string closed_trade_entry_id(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::string();
        return trades_[idx].entry_id;
    }
    std::string closed_trade_exit_id(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::string();
        return trades_[idx].exit_id;
    }
    double closed_trade_entry_price(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::numeric_limits<double>::quiet_NaN();
        return trades_[idx].entry_price;
    }
    double closed_trade_exit_price(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::numeric_limits<double>::quiet_NaN();
        return trades_[idx].exit_price;
    }
    int64_t closed_trade_entry_time(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0;
        return trades_[idx].entry_time;
    }
    int64_t closed_trade_exit_time(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0;
        return trades_[idx].exit_time;
    }
    double closed_trade_size(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::numeric_limits<double>::quiet_NaN();
        return trades_[idx].qty;
    }
    double closed_trade_max_runup(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0.0;
        return trades_[idx].max_runup;
    }
    double closed_trade_max_runup_percent(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0.0;
        const Trade& t = trades_[idx];
        double cost = t.entry_price * t.qty;
        return (cost > 0.0) ? (t.max_runup / cost) * 100.0 : 0.0;
    }
    double closed_trade_max_drawdown(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0.0;
        return trades_[idx].max_drawdown;
    }
    double closed_trade_max_drawdown_percent(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0.0;
        const Trade& t = trades_[idx];
        double cost = t.entry_price * t.qty;
        return (cost > 0.0) ? (t.max_drawdown / cost) * 100.0 : 0.0;
    }

    // --- Direction accessors ---
    std::string closed_trade_direction(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return "";
        return trades_[idx].is_long ? "long" : "short";
    }
    std::string open_trade_direction(int idx) const {
        if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size()) return "";
        return (position_side_ == PositionSide::LONG) ? "long" : "short";
    }

    // --- Open position trade accessors (strategy.opentrades.*) ---
    double open_trade_profit(int idx) const;
    double open_trade_profit_percent(int idx) const;
    double open_trade_commission(int idx) const;
    int open_trade_entry_bar_index(int idx) const;
    std::string open_trade_entry_comment(int idx) const;
    std::string open_trade_entry_id(int idx) const;
    double open_trade_entry_price(int idx) const;
    int64_t open_trade_entry_time(int idx) const;
    double open_trade_size(int idx) const;
    double open_trade_max_drawdown(int idx) const;
    double open_trade_max_drawdown_percent(int idx) const;
    double open_trade_max_runup(int idx) const;
    double open_trade_max_runup_percent(int idx) const;

private:
    double calc_qty_for_type(double fill_price, double qty_value, int qty_type) const;
    void execute_market_entry(const std::string& id, bool is_long, double fill_price,
                              double explicit_qty = std::numeric_limits<double>::quiet_NaN(),
                              int explicit_qty_type = -1,
                              PositionSide created_position_side = PositionSide::FLAT,
                              bool close_only_opposite = false,
                              bool is_priced_entry = false,
                              double tv_carry_qty = 0.0,
                              int created_bar = -1);
    void execute_market_exit(double fill_price);
    void execute_partial_exit_qty(double fill_price, double qty_to_close);
    void execute_partial_exit(double fill_price, double qty_percent);
    void execute_partial_exit_by_entry(double fill_price, const std::string& from_entry);
    void execute_partial_exit_by_entry_percent(double fill_price, const std::string& from_entry, double qty_percent);
    void cancel_oca_group(const std::string& oca_name, const std::string& exclude_id);
    // Pine v6 oca.reduce: when one sibling fills qty Q, reduce remaining
    // siblings' qty by Q. Siblings whose qty becomes <= 0 are cancelled.
    void reduce_oca_group(const std::string& oca_name, const std::string& exclude_id,
                          double filled_qty);
    void purge_exit_orders();

    // process_pending_orders helpers (defined in engine_fills.cpp).
    // Decomposed during the function-decomposition refactor so the
    // bar-pump fill loop is reviewable rather than a 600-line monolith.
    void update_trail_best_for_bar_open(const Bar& bar);
    void sort_exit_siblings_by_path_fill(const Bar& bar);
    void sort_orders_by_fill_phase(const Bar& bar);
    void compact_filled_pending_orders(const std::vector<size_t>& filled_indices,
                                       int exit_closed_from_bar,
                                       bool exit_closed_was_long);
    // Apply a fill to engine state: dispatches by order.type to the
    // per-type apply_*_order_fill helpers below, plus runs the risk
    // gate, intraday-fill cap, OCA cancellation, and bookkeeping that
    // is common to every fill kind.
    void apply_filled_order_to_state(PendingOrder& order,
                                     size_t order_index,
                                     double fill_price,
                                     const Bar& bar,
                                     double& trail_best_path_state,
                                     int& exit_closed_from_bar,
                                     bool& exit_closed_was_long,
                                     std::vector<size_t>& filled_indices);
    // Per-OrderType fill kernels. Called only after risk + intraday
    // gates pass; each updates the engine's position/trade state and
    // any per-type out-parameters the post-fill bookkeeping needs.
    void apply_market_order_fill(PendingOrder& order, double fill_price,
                                 const Bar& bar,
                                 double& trail_best_path_state);
    void apply_entry_order_fill(PendingOrder& order, double fill_price,
                                const Bar& bar,
                                double& trail_best_path_state);
    void apply_exit_order_fill(PendingOrder& order, double fill_price,
                               int& exit_closed_from_bar,
                               bool& exit_closed_was_long);
    void apply_raw_order_fill(PendingOrder& order, double fill_price,
                              double& trail_best_path_state,
                              int& exit_closed_from_bar,
                              bool& exit_closed_was_long);

    // Inner-loop phase split for process_pending_orders.
    // The inner loop iterates `pending_orders_` and processes each via
    // 3 phases: eligibility (should we even consider this order?),
    // fill-price (if eligible, what price would it fill at?), and
    // apply (mutate engine state with the fill — see apply_*_order_fill
    // declarations above).
    enum class OrderEligibility { Proceed, Skip, Remove };
    OrderEligibility classify_order_eligibility(
        PendingOrder& order, int opposing_pass,
        internal::DualEntryStopPathWinner dual_entry_path,
        const std::unordered_set<std::string>& pass0_opposing_skip_ids,
        int exit_closed_from_bar, bool exit_closed_was_long);
    struct FillEvaluation {
        enum class Kind { Fill, NoFill, DeferredToOpposingPass };
        Kind kind;
        double fill_price;
    };
    FillEvaluation evaluate_fill_price(
        PendingOrder& order, size_t order_index, const Bar& bar,
        int opposing_pass, double trail_best_path_state,
        std::unordered_set<std::string>& pass0_opposing_skip_ids);

    // strategy_close / strategy_exit helpers (defined in
    // engine_strategy_commands.cpp).
    bool compute_close_target_qty(const std::string& id,
                                  double qty,
                                  double qty_percent,
                                  double& matching_qty_out,
                                  double& qty_to_close_out,
                                  bool& all_entries_match_out);
    void cancel_orders_for_full_close(const std::string& id, bool closing_long);
    void execute_immediate_close(const std::string& id,
                                 const std::string& comment,
                                 double qty_to_close,
                                 double matching_qty,
                                 bool closes_full_position,
                                 bool closes_fifo_qty,
                                 bool closes_any_qty);
    void queue_deferred_close_order(const std::string& id,
                                    const std::string& comment,
                                    double qty_to_close,
                                    double matching_qty,
                                    bool closes_full_position,
                                    bool closes_any_qty);
    void clear_existing_exit_order(const std::string& id,
                                   const std::string& from_entry,
                                   bool has_trail_request,
                                   int64_t& preserved_seq_out,
                                   double& preserved_reserved_qty_out);
    bool compute_exit_reserved_qty(const std::string& from_entry,
                                   double preserved_reserved_qty,
                                   double& qp_io,
                                   bool& is_partial_io,
                                   double& reserved_qty_out);

    // execute_market_entry / execute_partial_exit_* helpers (defined in
    // engine_orders.cpp).
    void emit_close_trade(const PyramidEntry& pe, double close_qty,
                          double fill_price, bool was_long);
    void reset_position_state_to_flat();
    void settle_position_after_partial_exit();
    void enter_market_from_flat(const std::string& id, bool is_long,
                                double fill_price, double explicit_qty,
                                int explicit_qty_type,
                                PositionSide created_position_side,
                                bool is_priced_entry, double tv_carry_qty,
                                int created_bar);
    void add_to_pyramid_market(const std::string& id, bool is_long,
                               double fill_price, double explicit_qty,
                               int explicit_qty_type,
                               PositionSide created_position_side,
                               bool is_priced_entry);
    void close_opposite_then_enter(const std::string& id, bool is_long,
                                   double fill_price, double explicit_qty,
                                   int explicit_qty_type);
    void flip_market_position_to(const std::string& id, bool is_long,
                                 double fill_price, double explicit_qty,
                                 int explicit_qty_type);
    void open_fresh_position(PositionSide requested, double fill_price,
                             double qty, const std::string& id);
    void consume_tv_carry_from_siblings(const std::string& id,
                                        PositionSide created_position_side,
                                        int created_bar);

    // run() helpers (defined in engine_run.cpp).
    int  count_expected_script_bars(const Bar* input_bars, int n_input,
                                    bool needs_aggregation) const;
    void init_security_eval_states_for_run(const std::string& effective_input_tf);
    void run_simple_bar_loop(const Bar* input_bars, int n_input);
    void run_aggregation_bar_loop(const Bar* input_bars, int n_input,
                                  bool bar_magnifier, int expected_script_bars);

    // fill_report helpers (defined in engine_report.cpp).
    void fill_trades_section(ReportC* out) const;
    void fill_security_diag_section(ReportC* out) const;
    void fill_trace_section(ReportC* out) const;

public:
    virtual ~BacktestEngine() = default;
    virtual void on_bar(const Bar& bar) = 0;

    void run(const Bar* bars, int n);

    void run(const Bar* input_bars, int n_input,
             const std::string& input_tf,
             const std::string& script_tf,
             bool bar_magnifier = false,
             int magnifier_samples = 4,
             MagnifierDistribution magnifier_dist = MagnifierDistribution::ENDPOINTS);

    void run(const Bar* input_bars, int n_input,
             const std::string& input_tf,
             const std::string& script_tf,
             const std::unordered_map<std::string, std::string>& inputs,
             const SymInfo& syminfo,
             const StrategyOverrides* overrides = nullptr,
             bool bar_magnifier = false,
             int magnifier_samples = 4,
             MagnifierDistribution magnifier_dist = MagnifierDistribution::ENDPOINTS);

    int trade_count() const { return (int)trades_.size(); }
    const Trade& get_trade(int i) const { return trades_[i]; }
    void fill_report(ReportC* out) const;
    static void free_report(ReportC* report);

    // Returns the error message captured by the most recent run() if it
    // failed, or an empty string if the run completed normally. Cleared at
    // the start of every run(). The C ABI exposes this via
    // pf_strategy_get_last_error().
    const std::string& last_error() const { return last_error_; }

    // Per-input override (title -> serialized value). Must be set before run()
    // so get_input_*() lookups pick up the TV-tester value rather than the
    // Pine default.
    void set_input(const std::string& key, const std::string& value) {
        inputs_[key] = value;
    }
    void clear_inputs() { inputs_.clear(); }
    void set_trade_start_time(int64_t timestamp_ms) {
        trade_start_time_ = timestamp_ms;
    }

    // Toggle volume-weighted per-sub-bar sampling inside run_magnified_bar.
    // Has no effect unless bar magnifier is enabled.
    void set_magnifier_volume_weighted(bool on) {
        magnifier_volume_weighted_ = on;
    }

    // --- Runtime trace API ---
    // Default off so existing strategies pay zero cost. The validator
    // flips this on per-strategy via ``strategy_set_trace_enabled`` (the
    // FFI shim defined in engine.cpp) before running a backtest whose
    // per-bar values it wants to cross-reference against TradingView.
    void set_trace_enabled(bool on) { trace_enabled_ = on; }
    bool trace_enabled() const { return trace_enabled_; }

    // Push a typed per-bar value into the trace buffer. Cheap when
    // disabled — a single bool branch and return. When enabled, name
    // interning amortises to a single hash lookup per call after the
    // first occurrence; the actual record is a 24-byte POD push_back.
    //
    // The bool / int overloads internally cast to double so the
    // transpiler pragma can emit a single call shape regardless of the
    // source variable's Pine type — keeping codegen rewrites trivial.
    void trace(const std::string& name, double value);
    void trace(const std::string& name, bool value)  { trace(name, value ? 1.0 : 0.0); }
    void trace(const std::string& name, int value)   { trace(name, static_cast<double>(value)); }
};

} // namespace pineforge
