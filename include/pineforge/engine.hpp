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
#include "series.hpp"
#include "timeframe.hpp"
#include "magnifier.hpp"
#include "session_time.hpp"
// Suppress per-strategy function declarations (strategy_create, run_backtest,
// etc.) whose pf_*_t parameter types conflict with the internal C++ types
// used in codegen-emitted extern "C" blocks that include this header.
// NOTE: this macro leaks into every TU that includes engine.hpp; include
// pineforge.h FIRST in any TU that needs the per-strategy declarations
// (see src/c_abi.cpp).
#define PINEFORGE_NO_STRATEGY_DECLS
// Angle-bracket form is the installed public path (deliberate).
#include <pineforge/pineforge.h>

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
    // Intrabar-fill excursion masks: when a priced (stop/limit) entry fills
    // mid-bar, the portion of the bar's range traversed BEFORE the fill is
    // not part of the trade's excursion (TV convention). On the assumed
    // OHLC path (bar_path_uses_high_first), an extreme that occurs before
    // the fill position is excluded from update_per_trade_extremes sampling
    // for the fill bar only. Both default false (market/open fills sample
    // the full bar).
    bool skip_entry_bar_high = false;
    bool skip_entry_bar_low = false;
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
    double commission = 0.0;
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
    double commission;           // mirrors pf_trade_t tail; semantics documented in pineforge.h
    int32_t entry_bar_index;
    int32_t exit_bar_index;
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
    pf_metrics_t metrics;
    pf_equity_point_t* equity_curve;
    int64_t equity_curve_len;
};

enum class OrderType { MARKET, ENTRY, EXIT, RAW_ORDER };

struct PendingOrder {
    std::string id;
    std::string from_entry;    // for exit orders
    OrderType type;
    bool is_long;
    double limit_price;        // NaN = not set
    double stop_price;         // NaN = not set
    double trail_points;       // NaN = not set (entry-relative activation, in ticks)
    // NaN = not set (absolute activation price level). Default-initialized so
    // direct PendingOrder constructions that never assign it (entry/order
    // orders, test fixtures) cannot read an indeterminate value through the
    // trail predicates.
    double trail_price = std::numeric_limits<double>::quiet_NaN();
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
    // Per-instrument quantity step (syminfo.* "qty_step" — the smallest
    // tradable lot increment, e.g. 0.0004 for BINANCE:ETHUSDT.P). 0 = disabled
    // (the engine default), so no quantity quantization is applied — corpus
    // instruments leave this 0 and are byte-identical. Only the forced-
    // liquidation (margin call) path floors its computed lot to this step to
    // mirror TradingView, which nibbles the position in exact lot multiples.
    double qty_step = 0.0;
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
    // Position is FLAT until the first entry fires; the canonical
    // accessor ``signed_position_size`` already reads as 0 when FLAT
    // regardless of this default, but several internal carry- and
    // risk-gating reads (strategy_entry's tv_carry_qty capture,
    // check_risk_allow_entry's max-position check) read position_qty_
    // directly. A non-zero default leaks into those reads on the very
    // first call of any session, producing phantom carry growth (probe
    // 62 trade #1 fired qty=2 from a default-leaked carry=1) and
    // spuriously blocked entries when ``risk_max_position_size_=1``.
    // Initialising to 0 keeps the canonical and direct reads aligned.
    double position_qty_ = 0.0;
    int position_entry_count_ = 0;  // number of entries in current direction (for pyramiding)
    int position_open_bar_ = -1;    // bar_index_ when position was opened (for exit delay)
    std::vector<PyramidEntry> pyramid_entries_;  // individual entries for trade reporting
    // Per-entry-id UNCLOSED quantity ledger, used ONLY by strategy.close(id)
    // under the default FIFO close-entries rule to decide how much to close.
    //
    // TradingView's strategy.close(id) closes the quantity of the entries
    // tagged `id` that have NOT already been targeted by a prior close(id) —
    // it does NOT re-sum the physical open lots. That distinction is
    // invisible when each id maps to one lot (the common case), but it is
    // load-bearing for strategies that re-use one entry id across sequential
    // buy/sell cycles (grid bots): there, the FIFO trade-record drain removes
    // the OLDEST physical lot — which may carry a different id — leaving the
    // id-tagged lot physically present. Summing physical lots would then
    // double-count it on the next close(id) and over-close. This ledger
    // tracks "entered qty for id minus already-closed-by-close(id) qty for
    // id", so close(id) closes exactly the right amount (the whole position
    // for a same-id DCA pyramid; one slot for a grid cycle). It is never read
    // under the ANY rule (which closes id-matched physical lots directly).
    std::unordered_map<std::string, double> id_unclosed_qty_;

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
    // Per-instrument lot-size step for forced-liquidation quantization.
    // 0 = disabled (default; corpus no-op). Fed via the syminfo_metadata
    // channel ("qty_step") or the SymInfo struct on the explicit run() path.
    // process_margin_call floors each liquidation lot DOWN to a multiple of
    // this, matching TradingView's per-instrument margin-call lot sizing.
    double qty_step_ = 0.0;
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

    // Account-currency FX multiplier for the broker affordability gate. When a
    // strategy declares ``currency=currency.XXX`` differing from the symbol's
    // quote currency (e.g. currency.INR on a USDT-quoted perp), TradingView
    // denominates equity in the account currency but the position notional in
    // the quote currency, converting the latter via the account-currency FX
    // rate before the ``required_margin <= equity`` check. The engine otherwise
    // assumes account == quote (FX 1.0). Injected via the syminfo metadata
    // channel (key "account_currency_fx"); defaults to 1.0 so every corpus
    // strategy (which never sets it) is byte-identical.
    double account_currency_fx_ = 1.0;

    // TradingView force-liquidation (margin call) toggle. TV runs the broker
    // margin-call emulator by default, so this defaults ON to match TV. It is
    // a no-op for the validation corpus (long-only positions at the default
    // 100% margin can never be liquidated — the formula denominator
    // ``margin/100 - direction`` is 0 — and no corpus short is sized at full
    // equity), and can be switched off via ``set_margin_call_enabled`` for
    // callers that want the legacy hold-to-infinity behaviour.
    bool margin_call_enabled_ = true;

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
    int64_t last_bar_time_ = 0;
    int last_bar_index_ = 0;
    // Chart's display timezone — separate from ``syminfo_.timezone`` (the
    // exchange TZ). Set by ``set_chart_timezone`` / the C ABI's
    // ``strategy_set_chart_timezone``. See the doc on ``set_chart_timezone``
    // for why these two TZ slots must NOT alias.
    std::string chart_timezone_;
    std::unordered_map<std::string, std::string> inputs_;

    // Injected symbol metadata (syminfo.shares_outstanding_*,
    // recommendations_*, target_price_*, pricescale, minmove, …). These
    // have no source in an OHLCV feed, so the engine returns na<double>()
    // unless a data feed pushes a value via ``set_syminfo_metadata``. Keyed
    // by the Pine member name (e.g. "shares_outstanding_total").
    std::unordered_map<std::string, double> syminfo_metadata_;

    // Input injection helpers for generated code
    double get_input_double(const std::string& key, double default_val) const;
    int get_input_int(const std::string& key, int default_val) const;
    int64_t get_input_int64(const std::string& key, int64_t default_val) const;
    bool get_input_bool(const std::string& key, bool default_val) const;
    std::string get_input_string(const std::string& key, const std::string& default_val) const;
    // input.source: resolve a runtime override string ("open"/"high"/"low"/
    // "close"/"volume"/"hl2"/"hlc3"/"ohlc4"/"hlcc4") to the matching native
    // source series. Returns ``default_series`` (the codegen-resolved defval
    // series) when the key is absent OR the override string is non-native —
    // the analyzer hard-rejects non-native defvals, so a non-native override
    // can only arrive from an operator-supplied input value; never crash on it.
    const Series<double>& get_input_source(const std::string& key,
                                           const Series<double>& default_series) const;

    // syminfo.* fundamental/exchange metadata that has no OHLCV source.
    // Returns the value injected via ``set_syminfo_metadata`` for ``key``,
    // or na<double>() when none was injected. Codegen routes the
    // na-by-default SYMINFO_MEMBER_MAP double fields here.
    double get_syminfo_metadata(const std::string& key) const {
        auto it = syminfo_metadata_.find(key);
        return it != syminfo_metadata_.end() ? it->second : na<double>();
    }

    // --- Native source-series history (input.source) ---
    // input.source supports runtime override of WHICH price series feeds an
    // indicator. The generated subclass only materializes ``_s_<field>`` for
    // fields whose history it subscripts, so those cannot back a runtime
    // override to an arbitrary native source. These base-class series are the
    // canonical, always-resolvable backing store. They are advanced exactly
    // once per script bar (same cadence as the subclass ``_s_<field>``) by
    // ``_push_source_series()`` and only when ``_src_series_active_`` — the
    // generated ctor sets it true iff the script uses at least one
    // input.source, so scripts that don't pay nothing but the (small) member
    // footprint.
    bool _src_series_active_ = false;
    Series<double> _src_open_;
    Series<double> _src_high_;
    Series<double> _src_low_;
    Series<double> _src_close_;
    Series<double> _src_volume_;
    Series<double> _src_hl2_;
    Series<double> _src_hlc3_;
    Series<double> _src_ohlc4_;
    Series<double> _src_hlcc4_;

    // Advance every native source series by the current bar. Mirrors the
    // subclass ``_s_<field>`` idiom: push on the first tick, update intrabar
    // (magnifier). Called at each on_bar dispatch point; no-op when inactive.
    void _push_source_series() {
        if (!_src_series_active_) return;
        const double o = current_bar_.open;
        const double h = current_bar_.high;
        const double l = current_bar_.low;
        const double c = current_bar_.close;
        const double v = current_bar_.volume;
        const double hl2   = (h + l) / 2.0;
        const double hlc3  = (h + l + c) / 3.0;
        const double ohlc4 = (o + h + l + c) / 4.0;
        const double hlcc4 = (h + l + c + c) / 4.0;
        if (is_first_tick_) {
            _src_open_.push(o);   _src_high_.push(h);   _src_low_.push(l);
            _src_close_.push(c);  _src_volume_.push(v);
            _src_hl2_.push(hl2);  _src_hlc3_.push(hlc3);
            _src_ohlc4_.push(ohlc4); _src_hlcc4_.push(hlcc4);
        } else {
            _src_open_.update(o);   _src_high_.update(h);   _src_low_.update(l);
            _src_close_.update(c);  _src_volume_.update(v);
            _src_hl2_.update(hl2);  _src_hlc3_.update(hlc3);
            _src_ohlc4_.update(ohlc4); _src_hlcc4_.update(hlcc4);
        }
    }

    // --- Runtime state ---
    Bar current_bar_;
    int bar_index_ = 0;
    int64_t next_order_seq_ = 1;
    // TV: at most one priced ENTRY "open" event per bar; persists across
    // multiple process_pending_orders calls (bar magnifier) and dual-pass
    // opposing-stop resolution (see engine_fills.cpp).
    int priced_entry_activity_bar_ = -1;
    bool priced_entry_filled_this_bar_ = false;

    // Transient: true only while applying a priced (stop/limit/trail) fill
    // (apply_filled_order_to_state). emit_close_trade reads it to fold the
    // pre-exit-fill portion of the current bar's OHLC path into the closing
    // trade's excursion (per-bar sampling can't see the exit bar — the
    // pyramid entry is gone before the next update_per_trade_extremes).
    bool fold_exit_path_extremes_ = false;
    // Transient companion for TRAIL exits: the trail's best (peak) price at
    // fill time. The peak that armed the trailing stop is by definition a
    // pre-fill favorable excursion of the closing trade (TV reports
    // MFE == fill + offset == peak), but first_touch_position can't place a
    // trail fill on the bar path (the level is only active after the peak),
    // so emit_close_trade folds the peak directly. NaN = not a trail fill.
    double fold_exit_trail_peak_ = std::numeric_limits<double>::quiet_NaN();
    // Set by evaluate_fill_price: the just-evaluated exit fill fired on the
    // TRAIL leg (vs stop/limit/gap). Consumed by apply_filled_order_to_state
    // to reconstruct the trail peak above.
    bool last_exit_fill_was_trail_ = false;
    // Transient: true only while dispatching a LIMIT-triggered fill
    // (apply_filled_order_to_state). apply_fill_slippage reads it to route
    // limit fills onto the unslipped limit-or-better path (apply_limit_fill)
    // while market/stop/trail fills keep apply_slippage. Always false
    // outside the dispatch window, so strategy.close / end-of-run /
    // intraday-cap synthetic closes stay on the market (slipped) path.
    bool current_fill_is_limit_ = false;

    std::vector<Trade> trades_;
    std::vector<PendingOrder> pending_orders_;

    // strategy.exit partial orders are one-shot per open position for a given id
    std::unordered_set<std::string> consumed_partial_exit_ids_;

    // Reusable scratchpad for the per-call opposing-stop deferral set in
    // process_pending_orders. Holds the ids of flat-issued entry stops that
    // lost the intra-bar path race in pass 0 and are reconsidered in pass 1.
    // Cleared at the start of each process_pending_orders call; the retained
    // capacity avoids a fresh heap allocation 2-4x per bar. Typically tiny
    // (0-1 entries). Not state — must be empty across calls.
    std::unordered_set<std::string> scratch_skip_ids_;

    // Reusable scratch for process_pending_orders (capacity persists across
    // calls, mirroring scratch_skip_ids_). Always cleared before use.
    std::vector<size_t> scratch_filled_indices_;

    // --- Trailing stop state ---
    // Best favorable price since position entry (for trailing stop computation)
    double trail_best_price_ = std::numeric_limits<double>::quiet_NaN();

    // --- Intraday fill counter ---
    // Counts every fill processed by ``apply_filled_order_to_state`` on
    // the current chart-day. When the count reaches
    // ``max_intraday_filled_orders_`` the engine emits TV's synthetic
    // "Close Position (Max number of filled orders in one day)" exit at
    // the cap-triggering fill's price and LATCHES (intraday_cap_hit_)
    // until the chart-day rolls over. Once latched, ALL further fills
    // on that chart-day are silently rejected — TV's broker emulator
    // emits at most one cap-close per chart-day (probe 97b: 382
    // cap-closes across 13 months of data, ~one per chart-day where
    // the cap fires). Pre-latch the engine recharged the counter
    // after each cap-cycle, which over-fired cap-closes (3459 engine
    // vs 1957 TV trades on probe 97b). Pre-fix-fix the engine just
    // skipped fills past the cap, leaving the position carried open
    // across day boundaries.
    int intraday_fill_count_ = 0;
    int intraday_day_ = -1;       // day key (dayofmonth*100+month) for reset
    bool intraday_cap_hit_ = false;  // latched once per chart-day; reset on day rollover

    // True iff the intraday cap is currently latched on the CURRENT bar's
    // chart-day. Performs a lazy day-rollover reset so callers outside the
    // fill path (notably ``strategy_entry`` / ``strategy_order``) see a
    // consistent view: TV silently drops *order placement* during the
    // latched window in addition to dropping fills (Pine docs: "all
    // subsequent orders are blocked until the start of the next trading
    // day"). Without the placement-time gate, an entry placed during the
    // latched bar (e.g., bar 04-06 23:45 with arm_long true while the
    // cap fired earlier on 04-06 07:00) would carry into the next chart-
    // day and fire on the first new-day bar (04-07 00:00) at a price TV
    // never reports, fabricating a phantom trade. Probe 97 trades #22..
    // are the canonical victim — the residual exit-price drift after the
    // 97a/97b composition fixes was driven by these phantom new-day
    // entries followed by mismatched cap-close exit prices below.
    bool _intraday_cap_currently_latched() {
        if (max_intraday_filled_orders_ <= 0) return false;
        BarTime bt = _decompose_bar_time_chart_tz();
        int cur_day = bt.dayofmonth * 100 + bt.month;
        if (cur_day != intraday_day_) {
            intraday_day_ = cur_day;
            intraday_fill_count_ = 0;
            intraday_cap_hit_ = false;
        }
        return intraday_cap_hit_;
    }

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

    // --- Per-script-bar equity curve (metrics + pf_report_t exposure) ---
    std::vector<pf_equity_point_t> equity_curve_;
    int64_t bars_in_market_ = 0;     // script bars with an open position at close
    double first_bar_open_ = std::numeric_limits<double>::quiet_NaN();  // buy&hold basis

    // --- Position-size extremes (strategy.max_contracts_held_*) ---
    double max_contracts_held_all_ = 0.0;
    double max_contracts_held_long_ = 0.0;
    double max_contracts_held_short_ = 0.0;

    // --- Even-trade counter (strategy.eventrades) ---
    int eventrades_count_ = 0;

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
                       const std::string& comment = "",
                       double qty = std::numeric_limits<double>::quiet_NaN(),
                       const std::string& oca_name = "");
    void strategy_cancel(const std::string& id);
    void strategy_cancel_all();
    void strategy_order(const std::string& id, bool is_long, double qty,
                        double limit_price = std::numeric_limits<double>::quiet_NaN(),
                        double stop_price = std::numeric_limits<double>::quiet_NaN(),
                        const std::string& oca_name = "",
                        int oca_type = 0);

    void process_pending_orders(const Bar& bar);

    // TradingView forced-liquidation (margin call). Run once per script bar
    // after order processing: if the bar's adverse extreme (high for shorts,
    // low for longs) breaches the open position's required margin, force-close
    // 4x the minimum qty needed to restore margin at the liquidation price.
    void process_margin_call(const Bar& bar);

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
    //
    // The 1e-9 epsilon nudge guards against FP slop: a price computed as
    // ``user_close + 0.5`` may land at ``1803.1199999998`` (just below the
    // 1803.12 mintick boundary), which a raw ``ceil`` would push to 1803.13
    // and a raw ``floor`` to 1803.11. The nudge resolves any value within
    // 1 nanotick of an exact mintick boundary to that boundary, keeping
    // the bias one-sided only for sub-mintick midpoints (e.g. 1804.945).
    double round_to_mintick_directional(double price, bool is_long_stop) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        constexpr double kBoundaryEps = 1e-9;
        double r = price / syminfo_mintick_;
        if (is_long_stop) {
            return std::ceil(r - kBoundaryEps) * syminfo_mintick_;
        }
        return std::floor(r + kBoundaryEps) * syminfo_mintick_;
    }

    double apply_slippage(double price, bool is_buy) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        // TradingView snaps fills to mintick directionally even when slippage
        // is zero: a buy fills at the next-higher mintick, a sell at the
        // next-lower mintick. The legacy nearest-mintick rounding biased
        // sub-mintick stop levels (e.g. (open+high)/2 for an odd-mintick
        // bar) up by one tick for sells, producing a deterministic +0.01
        // exit-price drift on the magnifier-dist corpus (≈ 180 trades per
        // probe). Matching TV's directional snap removes that drift while
        // preserving the original add-slippage-then-snap shape for the
        // slippage > 0 path.
        if (slippage_ == 0) {
            return round_to_mintick_directional(price, /*is_long_stop=*/is_buy);
        }
        double slip = slippage_ * syminfo_mintick_;
        double slipped = is_buy ? price + slip : price - slip;
        return round_to_mintick_directional(slipped, /*is_long_stop=*/is_buy);
    }

    // TradingView applies slippage to MARKET and STOP fills but NOT to
    // LIMIT fills: a limit order fills at limit-or-better. An off-tick
    // limit price snaps one tick in the FAVORABLE direction (sell limit
    // -> ceil, buy limit -> floor) — the opposite direction of the
    // adverse market/stop snap in apply_slippage. A limit order that gaps
    // through at the bar open fills at the raw open (better price), also
    // unslipped; the open is on-tick in practice so the favorable snap is
    // an identity there.
    //
    // Evidence (2026-06-12): TV export of corpus/validation/
    // bracket-exit-tp-sl-fixed-01 on BINANCE:ETHUSDT.P, commission 0.1%,
    // slippage 2, mintick 0.01 — TP (limit) exits: 152/152 intra-bar
    // fills equal ceil(limit) with no slip (62 of them discriminate ceil
    // from round-to-nearest), 44/44 gap fills equal the raw bar open.
    // SL (stop) exits 195/195 and market entries 396/396 match the
    // slipped path, pinning slippage to market/stop fills only. The
    // probe's slippage=0 tv_trades.csv shows the same favorable snap
    // (143/143 TP fills at ceil(limit)), so this rule is slippage-
    // independent.
    double apply_limit_fill(double price, bool is_buy) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        return round_to_mintick_directional(price, /*is_long_stop=*/!is_buy);
    }

    // Fill-time dispatcher: LIMIT-triggered fills take the unslipped
    // limit-or-better path, everything else (market/stop/trail) takes
    // apply_slippage. current_fill_is_limit_ is the transient set around
    // the per-order fill dispatch in apply_filled_order_to_state.
    double apply_fill_slippage(double price, bool is_buy) const {
        return current_fill_is_limit_ ? apply_limit_fill(price, is_buy)
                                      : apply_slippage(price, is_buy);
    }

    // --- Commission helper ---
    // PERCENT commission is a % of the order's notional value in account
    // currency, which for futures includes the instrument point value
    // (price points × $/point × contracts). pointvalue=1 (crypto/equity
    // default) leaves the legacy formula bit-identical. Cash-per-order /
    // cash-per-contract are already absolute currency amounts.
    double calc_commission(double fill_price, double qty) const {
        switch (commission_type_) {
            case CommissionType::PERCENT:
                return fill_price * qty * syminfo_.pointvalue * (commission_value_ / 100.0);
            case CommissionType::CASH_PER_ORDER:
                return commission_value_;
            case CommissionType::CASH_PER_CONTRACT:
                return commission_value_ * qty;
        }
        return 0.0;
    }

    // --- Position sizing helper ---
    // PERCENT_OF_EQUITY / CASH convert a currency budget into contracts, so
    // one contract's currency exposure is fill_price × pointvalue (futures
    // $-per-point multiplier; 1.0 for crypto/equity keeps the legacy math
    // bit-identical).
    double calc_qty(double fill_price) const {
        switch (default_qty_type_) {
            case QtyType::FIXED:
                return default_qty_value_;
            case QtyType::PERCENT_OF_EQUITY: {
                double equity = current_equity();
                double cash = equity * (default_qty_value_ / 100.0);
                // Reject (qty 0) on a non-finite / non-positive fill price — a
                // degenerate $0/NaN print must NOT size as the raw % number.
                return (std::isfinite(fill_price) && fill_price > 0)
                    ? (cash / (fill_price * syminfo_.pointvalue)) : 0.0;
            }
            case QtyType::CASH:
                return (std::isfinite(fill_price) && fill_price > 0)
                    ? (default_qty_value_ / (fill_price * syminfo_.pointvalue)) : 0.0;
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
    // strategy.margin_liquidation_price — the price at which TradingView's
    // broker emulator force-liquidates the current open position. Returns na
    // when flat, when the instrument has no valid size/point-value, or when
    // ``margin/100 - direction == 0`` (a long at 100% margin can never be
    // liquidated). See compute_liquidation_price for the derivation.
    double margin_liquidation_price() const { return compute_liquidation_price(); }

    // Shared liquidation-price formula (TradingView docs, validated against the
    // p2 margin-call probe and the leverage-margin-call-perp-5x corpus probe):
    //
    //   liqPrice = ((initial_capital + net_profit) / (pointvalue * |size|)
    //               - direction * entry) / (margin_pct/100 - direction)
    //
    //   direction = +1 long / -1 short; net_profit = realized closed-trade PnL;
    //   entry = current average entry price; size = open position size.
    //
    // Rounded UP to mintick for shorts, DOWN for longs (TV convention).
    double compute_liquidation_price() const {
        if (position_side_ == PositionSide::FLAT) return na<double>();
        const double pv = syminfo_.pointvalue;
        const double qty = position_qty_;
        if (!(qty > 0.0) || !(pv > 0.0)) return na<double>();
        const double direction = (position_side_ == PositionSide::LONG) ? 1.0 : -1.0;
        const double margin_pct = (position_side_ == PositionSide::LONG)
                                      ? margin_long_ : margin_short_;
        const double denom = (margin_pct / 100.0) - direction;
        // A long at 100% margin (denom == 0) cannot be liquidated.
        if (std::abs(denom) < 1e-12) return na<double>();
        const double equity_basis = initial_capital_ + net_profit_sum_;
        double liq = (equity_basis / (qty * pv) - direction * position_entry_price_)
                     / denom;
        if (syminfo_mintick_ > 0.0) {
            liq = (position_side_ == PositionSide::SHORT)
                      ? std::ceil(liq / syminfo_mintick_) * syminfo_mintick_
                      : std::floor(liq / syminfo_mintick_) * syminfo_mintick_;
        }
        return liq;
    }
    double open_trades_capital_held() const {
        if (position_side_ == PositionSide::FLAT) return 0.0;
        return std::abs(position_qty_ * position_entry_price_) * syminfo_.pointvalue;
    }

    // Mark-to-market open profit in account currency. The point-value
    // multiplier keeps this consistent with realized PnL (emit_close_trade)
    // so equity = capital + net_profit + open_profit stays in one unit.
    double open_profit(double current_price) const {
        if (position_side_ == PositionSide::FLAT) return 0.0;
        double diff = (position_side_ == PositionSide::LONG)
            ? (current_price - position_entry_price_)
            : (position_entry_price_ - current_price);
        return diff * position_qty_ * syminfo_.pointvalue;
    }

    int count_wintrades() const { return win_trades_count_; }
    int count_losstrades() const { return loss_trades_count_; }

    // --- Time/date extraction from bar timestamp ---
    // Pine's bare ``hour`` / ``minute`` / ``dayofweek`` (the variable form,
    // not the 1-arg function form) returns the wall-clock for the **exchange
    // timezone** of the symbol (per TV reference docs). For crypto symbols
    // like ETH-USDT the exchange TZ is UTC, which matches the engine's
    // storage TZ — so the cheap ``gmtime_r`` path is correct for the
    // overwhelming majority of strategies in the corpus.
    //
    // The 1-arg function form ``hour(time)`` is handled separately by the
    // codegen (see codegen/visit_call.py) and DOES honour
    // ``syminfo_.timezone`` (set via ``strategy_set_chart_timezone``) since
    // TV's reference says the function form defaults its tz arg to
    // ``syminfo.timezone``, which TV harnesses commonly set to the chart's
    // display TZ for cross-exchange / multi-zone work.
    struct BarTime {
        int year, month, dayofmonth, hour, minute, second, dayofweek, weekofyear;
    };

    // Single-entry memo: generated scripts commonly read several time
    // components per bar (hour + minute + dayofweek); decompose once per
    // distinct bar timestamp instead of per accessor. Keyed on the raw
    // timestamp, so no per-run invalidation is needed (same ts -> same
    // UTC decomposition, run-independent).
    mutable int64_t bar_time_memo_ts_ = std::numeric_limits<int64_t>::min();
    mutable BarTime bar_time_memo_{};

    BarTime _decompose_bar_time() const {
        if (current_bar_.timestamp == bar_time_memo_ts_) return bar_time_memo_;
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
        bar_time_memo_ts_ = current_bar_.timestamp;
        bar_time_memo_ = bt;
        return bt;
    }

    // Chart-timezone-aware decomposition. ONLY for intraday-day rollover
    // gates (max_intraday_filled_orders, max_intraday_loss, consecutive
    // loss-day tracking) — those must roll over at the chart's wall-clock
    // 00:00, matching TV's broker emulator (which keys off the chart's
    // display TZ, not the exchange TZ).
    //
    // Falls back to plain ``_decompose_bar_time()`` (UTC) when no chart
    // timezone has been set, preserving the legacy fast path for
    // engine consumers that don't call ``set_chart_timezone``.
    //
    // Defined out-of-line in src/engine_risk.cpp so we can use the
    // private ``ScopedTimezone`` helper without leaking its header into
    // the public engine.hpp surface.
    BarTime _decompose_bar_time_chart_tz() const;

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

    // --- Session predicate bar-state tracking ---
    // Tracks whether the previous bar was inside the regular session.
    // Used to compute session.isfirstbar (in_session && !prev_in_session_)
    // and session.islastbar (prev_in_session_ && !in_session).
    bool prev_in_session_ = false;
    // Current-bar session predicates — recomputed at the start of each bar
    // by update_session_state() in engine_run.cpp.
    bool session_ismarket_ = false;
    bool session_isfirstbar_ = false;
    bool session_islastbar_ = false;

    // --- Timeframe state ---
    std::string input_tf_;
    std::string script_tf_;
    // Cached tf_to_seconds(script_tf_). MUST be refreshed immediately after
    // every assignment to script_tf_ (both sites live in engine_run.cpp).
    // Avoids a string parse per strategy.* call.
    int script_tf_seconds_ = 0;
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
        // Heikin-Ashi same-symbol read: request.security(ticker.heikinashi(
        // syminfo.tickerid), ...). When set, the completed (aggregated) bar's
        // OHLC is replaced by its Heikin-Ashi candle before the security
        // expression is evaluated, so close/open/high/low inside the call see
        // HA values. HA is stateful (ha_open depends on the prior HA bar), so
        // the running state lives here per sec_id.
        bool heikinashi = false;
        double ha_prev_open = 0.0;
        double ha_prev_close = 0.0;
        bool ha_seeded = false;
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
                                bool gaps_on = false, bool heikinashi = false);
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
    void run_magnified_bar(const std::vector<Bar>& sub_bars, int64_t script_bar_ts);
    virtual void finalize_bar() {}

    // --- Equity extremes update (called after each on_bar) ---
    // NOTE: the dd/runup walk in src/engine_metrics.cpp (compute_equity_stats)
    // MUST mirror this trough-reset logic; keep in lockstep.
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

        // --- Update max_contracts_held_* running peaks ---
        double abs_qty = std::abs(position_qty_);
        if (position_side_ != PositionSide::FLAT) {
            if (abs_qty > max_contracts_held_all_) max_contracts_held_all_ = abs_qty;
            if (position_side_ == PositionSide::LONG && abs_qty > max_contracts_held_long_)
                max_contracts_held_long_ = abs_qty;
            if (position_side_ == PositionSide::SHORT && abs_qty > max_contracts_held_short_)
                max_contracts_held_short_ = abs_qty;
        }
    }

    // Record one equity point per SCRIPT bar. ``script_bar_ts`` must be the
    // script-bar open timestamp captured BEFORE dispatch — current_bar_.timestamp
    // is overwritten by the magnifier sub-bar walk (engine_run.cpp), which would
    // make the curve differ between magnifier on/off.
    void record_equity_point(int64_t script_bar_ts) {
        if (equity_curve_.empty()) first_bar_open_ = current_bar_.open;
        pf_equity_point_t p;
        p.time_ms = script_bar_ts;
        p.open_profit = open_profit(current_bar_.close);
        p.equity = initial_capital_ + net_profit_sum_ + p.open_profit;
        equity_curve_.push_back(p);
        if (position_side_ != PositionSide::FLAT) ++bars_in_market_;
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
        return trades_[idx].commission;
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
    // Percent excursions: trade.max_runup / max_drawdown are stored in
    // account currency (× pointvalue, see emit_close_trade), so the entry
    // cost denominator must be in currency too (entry × qty × pointvalue).
    // pointvalue=1 cancels out and matches the legacy ratio bit-for-bit.
    double closed_trade_max_runup_percent(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0.0;
        const Trade& t = trades_[idx];
        double cost = t.entry_price * t.qty * syminfo_.pointvalue;
        return (cost > 0.0) ? (t.max_runup / cost) * 100.0 : 0.0;
    }
    double closed_trade_max_drawdown(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0.0;
        return trades_[idx].max_drawdown;
    }
    double closed_trade_max_drawdown_percent(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0.0;
        const Trade& t = trades_[idx];
        double cost = t.entry_price * t.qty * syminfo_.pointvalue;
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

    std::string position_entry_name() const {
        if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return "";
        return pyramid_entries_.back().entry_id;
    }

    double max_drawdown_percent() const {
        return (initial_capital_ > 0.0) ? (max_drawdown_ / initial_capital_) * 100.0 : 0.0;
    }

    int64_t time_close() const {
        return pine_time_close(current_bar_.timestamp, script_tf_, syminfo_.session, syminfo_.timezone, script_tf_);
    }

    // Internal sizing helper; protected (alongside calc_qty) so the sizing-guard
    // test can exercise the fill_price<=0 / NaN rejection path directly. See
    // tests/test_adversarial_ohlcv.cpp.
    double calc_qty_for_type(double fill_price, double qty_value, int qty_type) const;

private:
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
    void purge_exit_orders(bool retain_for_pending_entries = false);

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
                                     bool fill_is_limit,
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
        // True when the LIMIT leg produced the fill (exit limit, entry
        // limit, or the limit leg of an entry stop-limit) — routes the
        // fill onto the unslipped limit-or-better price path.
        bool is_limit_fill = false;
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
    // FIFO-drain up to qty_limit from pyramid_entries_, in order, splitting the
    // boundary entry as needed. When from_entry is non-null only entries whose
    // entry_id == *from_entry are eligible (others are kept untouched); null
    // drains across all entries. Emits one close Trade per drained slice at
    // fill_price (already slippage-adjusted) and rebuilds pyramid_entries_ /
    // decrements position_qty_ by the amount drained. Returns the total qty
    // drained. Shared by execute_partial_exit_qty and
    // execute_partial_exit_by_entry_percent.
    double fifo_drain(const std::string* from_entry, double qty_limit,
                      double fill_price, bool was_long);
    void reset_position_state_to_flat();
    // Reset ALL per-run state (trades, accumulators, position, pending orders,
    // equity extremes, risk latches, intraday/day counters, source-series
    // history) so a reused handle's run N is bit-identical to a fresh handle's
    // run 1. Preserves configuration (initial_capital_, pyramiding_, slippage_,
    // commission_*, default_qty_*, syminfo_, inputs_, risk thresholds) — those
    // are set before run() and must survive it. Called at the top of every
    // run() loop entrypoint. See tests/test_handle_reuse_reset.cpp.
    void reset_run_state();
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
    // Runs the standard per-script-bar order/strategy sequence on current_bar_:
    //   process_pending_orders -> update_per_trade_extremes -> on_bar,
    // plus a second process_pending_orders when process_orders_on_close_ is set
    // (TV process_orders_on_close: new market orders fill at this bar's close).
    // Shared by run(), run_simple_bar_loop, and the no-magnifier aggregation
    // path. The magnifier tick loop does NOT use this — it gates the sequence
    // on is_last_tick_ and forces is_first_tick_ before on_bar.
    void dispatch_bar();
    void run_simple_bar_loop(const Bar* input_bars, int n_input);
    void run_aggregation_bar_loop(const Bar* input_bars, int n_input,
                                  bool bar_magnifier, int expected_script_bars);

    // fill_report helpers (defined in engine_report.cpp).
    void fill_trades_section(ReportC* out) const;
    void fill_metrics_section(ReportC* out) const;
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

    // --- Position-size extremes (strategy.max_contracts_held_*) ---
    double max_contracts_held_all() const { return max_contracts_held_all_; }
    double max_contracts_held_long() const { return max_contracts_held_long_; }
    double max_contracts_held_short() const { return max_contracts_held_short_; }

    // --- Even-trade count (strategy.eventrades) ---
    int eventrades() const { return eventrades_count_; }

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

    // Set the chart's display timezone. Stored in a dedicated slot so it
    // does NOT clobber ``syminfo_.timezone`` (the symbol/exchange TZ).
    //
    // Pre-fix this method wrote the chart TZ into ``syminfo_.timezone``,
    // which the codegen reads as the default tz argument of the 1-arg
    // ``hour(time)`` / ``minute(time)`` / ``dayofweek(time)`` form. That
    // conflated two distinct TV concepts and silently shifted the result
    // by the chart-vs-exchange offset (e.g. Asia/Taipei vs UTC = +8h)
    // for crypto symbols. The shift cascaded into ``hour``-bucketed
    // accumulators — see
    // ``validation_typed_matrix/typed-matrix-probe-01-bool-regime-mask``,
    // whose 24x7 ``matrix<bool>`` regime mask filled in 8 hours earlier
    // than TV and produced ~9% trade-count divergence (TV 773, engine 714
    // before this fix; ~778 after).
    //
    // TV semantics (Pine v6 reference docs):
    //   * Bare variable ``hour`` / ``minute`` / ``dayofweek``: exchange
    //     timezone (``syminfo.timezone``). Already correct via
    //     ``_decompose_bar_time()``'s hardcoded ``gmtime_r``, which
    //     matches the corpus' ETH-USDT (UTC) data.
    //   * 1-arg function form ``hour(time)``: defaults its tz arg to
    //     ``syminfo.timezone`` (NOT the chart display TZ). With this
    //     change, ``syminfo_.timezone`` retains its constructor default
    //     ("UTC") and the codegen lambda lands on the cheap gmtime_r
    //     branch — matching TV.
    //   * 2-arg function form ``hour(time, tz)``: honours the explicit
    //     argument, unchanged by this fix.
    void set_chart_timezone(const std::string& tz) {
        chart_timezone_ = tz;
    }
    const std::string& chart_timezone() const { return chart_timezone_; }

    // --- Symbol metadata injection (data feed → syminfo.*) ---
    // The exchange timezone + session feed session.ismarket / time(session)
    // predicates. They default to UTC / 24x7 (crypto); a data feed pushes
    // the real values via these setters before run().
    //
    // NOTE on intraday-day rollover gates (max_intraday_filled_orders,
    // max_intraday_loss, consecutive-loss day): these intentionally key off
    // ``chart_timezone_`` (see ``_decompose_bar_time_chart_tz``), which is
    // what TV's broker emulator matched on the only validated case (probe-97,
    // crypto on a UTC+8 chart). For real-session instruments (e.g. US
    // equities), the serving layer should set ``set_chart_timezone`` to the
    // exchange timezone so the gate rolls over on the exchange trading day —
    // we deliberately do NOT switch the gates to ``syminfo_.timezone`` (that
    // would regress the crypto-on-shifted-chart case).
    void set_syminfo_timezone(const std::string& tz) { syminfo_.timezone = tz; }
    void set_syminfo_session(const std::string& s) { syminfo_.session = s; }
    // Runtime syminfo injection (by design — the engine stores no instrument
    // metadata of its own; the harness supplies it per run). mintick drives the
    // directional fill snap + slippage*tick economics; pointvalue is the
    // futures $-per-point multiplier applied to every money path (realized
    // PnL + excursions, open profit / equity, percent/cash sizing, percent
    // commission notionals, margin check — see tests/test_pointvalue.cpp).
    // Both default to crypto/equity values (0.01 / 1.0) and only matter when the
    // harness sets a non-default instrument.
    void set_syminfo_mintick(double m) { if (m > 0.0) { syminfo_.mintick = m; syminfo_mintick_ = m; } }
    void set_syminfo_pointvalue(double pv) { if (pv > 0.0) { syminfo_.pointvalue = pv; } }

    // Toggle TradingView's forced-liquidation (margin call) emulation. Defaults
    // ON to match TV; set false for the legacy hold-the-position behaviour.
    void set_margin_call_enabled(bool enabled) { margin_call_enabled_ = enabled; }
    bool margin_call_enabled() const { return margin_call_enabled_; }
    void set_syminfo_metadata(const std::string& key, double value) {
        syminfo_metadata_[key] = value;
        // "qty_step" is the per-instrument lot increment used by the forced-
        // liquidation quantizer. Route it onto the dedicated member so the
        // codegen run(const Bar*, int) path (which never overwrites it) keeps
        // the value the data feed injected. A non-positive value disables it.
        if (key == "qty_step") {
            qty_step_ = (std::isfinite(value) && value > 0.0) ? value : 0.0;
            syminfo_.qty_step = qty_step_;
        }
        // Account-currency FX rate (account-currency units per quote-currency
        // unit). Scales the broker affordability gate's required_margin when
        // the script's currency differs from the symbol quote currency. A
        // non-positive / non-finite value resets to the 1.0 (no-op) default.
        if (key == "account_currency_fx") {
            account_currency_fx_ =
                (std::isfinite(value) && value > 0.0) ? value : 1.0;
        }
    }

    // Returns the script's active timeframe string (e.g. "15" for 15-minute,
    // "D" for daily). Backs timeframe.main_period in generated Pine v6 code.
    const std::string& main_period() const { return script_tf_; }

    // Toggle volume-weighted per-sub-bar sampling inside run_magnified_bar.
    // Has no effect unless bar magnifier is enabled.
    void set_magnifier_volume_weighted(bool on) {
        magnifier_volume_weighted_ = on;
    }

    // --- Runtime trace API ---
    // Default off so existing strategies pay zero cost. The validator
    // flips this on per-strategy via ``strategy_set_trace_enabled`` (the
    // FFI shim defined in c_abi.cpp) before running a backtest whose
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
