#pragma once
#include <atomic>
#include <vector>
#include <string>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <limits>
#include <memory>
#include <set>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <optional>
#include <functional>
#include <pineforge/execution_consumer.hpp>
#include "na.hpp"
#include "bar.hpp"
#include "broker_events.hpp"
#include "quantity_intent.hpp"
#include "execution.hpp"
#include "execution_close_scope.hpp"
#include "execution_close_selection.hpp"
#include "execution_projection.hpp"
#include "execution_reverse_to.hpp"
#include "position_close_obligation.hpp"
#include "market_admission.hpp"
#include "order_cancellation.hpp"
#include "leg_activation.hpp"
#include "exit_leg_lifecycle.hpp"
#include "order_birth.hpp"
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

// Generated modules using the full script lifecycle reset must be rebuilt
// against a runtime providing this hook. This is an internal C++ capability;
// it does not change any public C POD or exported C function signature.
#define PINEFORGE_HAS_SCRIPT_RUN_PREPARE_V1 1
#define PINEFORGE_HAS_NATIVE_LIVE_V1 1
namespace pineforge {

enum class PositionSide { FLAT, LONG, SHORT };

// One physical emulator lot entry/exit, in actual execution order. A reversal
// may produce several exits followed by an entry. No range-end report rows.
struct StreamOrderAction {
    uint64_t sequence = 0;
    int64_t timestamp_ms = 0;
    int32_t bar_index = 0;
    bool is_entry = false;
    bool is_long = false;
    double quantity = 0.0;
    double price = 0.0;
    std::string order_id;
    std::string comment;
    uint64_t entry_incarnation = 0;
    size_t closed_trade_index = static_cast<size_t>(-1); // engine-private provenance
};

// Forward declaration of an internal enum used by some BacktestEngine
// method signatures. The full definition lives in src/engine_internal.hpp
// (private to libruntime); only the underlying-type pin is needed here.
namespace internal {
enum class DualEntryStopPathWinner : int;
}

// ────────────────────────────────────────────────────────────────────
// Host-owned per-lot excursion accounting (RULING A48, one generic
// capability).  A host that declares ownership of a lot's favorable/adverse
// excursion supplies the closed-lot result from its own sampler; the kernel
// then neither samples excursion at matched trigger prices nor folds bar-path
// extremes into the closing row.  Everything below is source-blind: the facts
// are the lot's own booking coordinates plus the carried extremes, and the
// result is the two price-difference x quantity magnitudes the row reports.
// ────────────────────────────────────────────────────────────────────
struct ClosedLotExcursionFacts {
    uint64_t entry_incarnation = 0;
    int64_t entry_time_ms = 0;
    double entry_price = 0.0;
    double lot_qty = 0.0;
    double closed_qty = 0.0;
    double fill_price = 0.0;
    double carried_favorable = 0.0;
    double carried_adverse = 0.0;
    bool is_long = true;
    int entry_bar_index = -1;
    int exit_bar_index = -1;
    // The lot's own entry-bar excursion masks (PyramidEntry): the booking
    // facts of a priced intrabar fill, carried across the boundary so the
    // owner never has to be reachable from the closing row.
    bool entry_bar_high_masked = false;
    bool entry_bar_low_masked = false;
};

struct ClosedLotExcursion {
    double favorable = 0.0;
    double adverse = 0.0;
};

using LotExcursionHook = std::function<ClosedLotExcursion(const ClosedLotExcursionFacts&)>;

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
    // KI-62: this slice opened as a same-direction MARKET pyramid add (not the
    // base open, not a priced entry). When a from_entry priced bracket exit
    // fills on this add's OWN entry bar and after it in TV's open-tick fill
    // sequence, the exit covers (scratches) the add dur-0. Gated by
    // entry_bar_index == bar_index_ so prior-bar slices are never covered.
    bool market_pyramid_add = false;
    // Synthetic OHLC-path coordinate where a pure stop/limit strategy.entry
    // fired.
    // A single flat-born, non-trailing from_entry bracket may inspect only the
    // path suffix at/after this cursor on the entry bar. NaN marks every
    // unrouted parent class (market, stop-limit, raw order).
    double entry_path_position = std::numeric_limits<double>::quiet_NaN();
    // Entry-leg commission in account currency at this slice's actual fill
    // boundary. Percentage commission depends on quote->account FX, so an
    // effective-time provider must not retroactively reprice this already-paid
    // fee when a later rate becomes active. Partial realization allocates this
    // paid cost proportionally and leaves only the unconsumed cost on the lot,
    // independently of the current fee schedule. NaN is reserved for legacy/
    // synthetic injection; every production entry path captures a real quote.
    double entry_commission_account =
        std::numeric_limits<double>::quiet_NaN();
    // Monotonic per-run identity of the request record object whose broker fill
    // created this physical lot. Unlike Pine's user-visible entry_id, an
    // incarnation is never reused by same-id replacements or later calls.
    // Every partial-close fragment copied from this lot therefore retains the
    // same physical-entry provenance. Zero is reserved for legacy/test-only
    // synthetic lots that were not created by a request record.
    uint64_t entry_incarnation = 0;
    // A foreign/global or ambiguous same-ID bracket consumed part of this
    // physical lot by FIFO. Its logical slot cannot later be released merely
    // because an owner-bound bracket closes the last physical remainder.
    bool bracket_slot_shadowed = false;
    // Exact ordinary MARKET fill at the next bar open. Priced/RAW entries
    // cannot infer this provenance from an equal numeric entry price.
    bool ordinary_market_open = false;
    // Actual flat-born MARKET fill at the ordinary POOC terminal close.
    // Priced/RAW entries and orders born in fill callbacks do not acquire it.
    bool pooc_terminal_market_entry = false;
    // A flat-born pure STOP strategy.entry actually filled at the bar open.
    // Keep this separate from MARKET provenance: equal fill prices do not
    // make the two order classes interchangeable for affordability rules.
    bool ordinary_stop_open = false;
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
    // True when this trade's exit fill came from a REAL strategy.exit
    // bracket leg (stop/limit/trail/profit/loss), as opposed to a
    // strategy.close/close_all market close, a reversal-driven close, a
    // margin-call slice, or an intraday-cap close. Set at two sites:
    //   1. The native applied-event projection classifies a live EXIT request
    //      whose id is not the adapter's internal "__close__" close command.
    //   2. Adapter receipt reconciliation preserves the classification for a
    //      bracket that survives a margin reduction.
    // ABI v4 task 9: closed_trade_close_cause() reads this to distinguish
    // BRACKET (2) from SCRIPT (1); it is never set on a margin-call /
    // intraday-cap row (those stay false and are classified from exit_id /
    // exit_comment instead).
    bool exit_from_bracket = false;
    double max_runup = 0.0;
    double max_drawdown = 0.0;
    double commission = 0.0;
    // Physical-entry provenance copied from PyramidEntry. This is deliberately
    // separate from entry_id: Pine permits user-visible IDs to be reused.
    uint64_t entry_incarnation = 0;
    // True for the range-end close of a position still open after the final
    // bar (record_range_end_close_trades); false for every script-driven
    // or bracket exit. Mirrors pf_trade_t::open_at_end.
    bool open_at_end = false;
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
    int32_t open_at_end;         // ABI v3: 1 on the range-end close row (pineforge.h)
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
    // ABI v4: per-script-bar broker-state hash (empty unless recording enabled).
    uint64_t* broker_state_hash;
    int64_t   broker_state_hash_len;
};

using ExitLegLifecycle = exit_legs::Lifecycle;

// default_qty_type constants (matches TradingView)
enum class QtyType { FIXED = 0, PERCENT_OF_EQUITY = 1, CASH = 2 };

// commission_type constants
enum class CommissionType { PERCENT = 0, CASH_PER_ORDER = 1, CASH_PER_CONTRACT = 2 };

// Pine user enum → str.tostring (field payloads). Transpiler enforces enum decl before
// input.enum; this clamps the index so bad values never read past the table.
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

// Value-owned runtime input transport shared by the public rich run overload
// and the generic native pre-begin provider.  It contains no frontend policy.
using InputsMap = std::unordered_map<std::string, std::string>;

// The C++ subclass contract is internal, unlike pineforge.h's stable C ABI.
// Changing its layout or vtable requires all generated/native C++ objects to be rebuilt.
// v6 adds explicit owner-bound exit-leg activation and Pine placement evidence.
// Version the mangled class name so older headers' member offsets/vtable cannot
// silently bind out-of-line members of this different object layout.
inline namespace engine_script_run_v18 {
class BrokerStateHashSink;
// Optional frontend projection interface retained for source compatibility.
// Broker dispatch itself is virtual on BacktestEngine and never discovers a
// host kind with RTTI.
class BrokerStateHashProvider {
public:
    virtual ~BrokerStateHashProvider() = default;
    virtual std::uint64_t broker_state_hash_projection() const = 0;
};
class BacktestEngine {
protected:
    // The consumer is the kernel's own execution authority, so this
    // friendship is also the access path for the request.security feed
    // machinery a native higher-timeframe subscription drives:
    // register_security_eval, prepare_native_security_feeds and
    // feed_security_eval_state stay protected members of this class and are
    // still out of reach of host code, while the consumer calls them at
    // begin and from its accepted-input path. No member moved and no
    // behaviour changed for this.
    friend class NativeExecutionConsumer;
    friend class NativeStrategyHost;
    struct NativeConsumerBindTag { explicit NativeConsumerBindTag() = default; };
    explicit BacktestEngine(NativeConsumerBindTag);
    IExecutionConsumer& execution_consumer();
    const IExecutionConsumer& execution_consumer() const;
    virtual void hash_source_extension(BrokerStateHashSink&) const;
    virtual std::uint64_t broker_state_hash_projection() const;
    std::uint64_t broker_state_hash_from_execution_hash(std::uint64_t) const;
    // --- Position state ---
    // @broker-state begin
    PositionSide position_side_ = PositionSide::FLAT;
    double position_entry_price_ = 0.0;   // volume-weighted average (for strategy calculations)
    // Owned, consumable post-fill checkpoint. The shared successful-fill
    // dispatcher supplies its producer and position identities; economic
    // eligibility is decided there, independently of this lifecycle model.
    broker::OpeningObligations opening_obligations_;
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
    // Exact position-instance provenance for pending orders whose semantics
    // depend on the position cycle in which they were placed. A fresh open or
    // reversal gets a new nonzero id; same-direction pyramid adds retain it.
    int64_t position_cycle_seq_ = 0;
    int64_t next_position_cycle_seq_ = 1;
    std::vector<PyramidEntry> pyramid_entries_;  // individual entries for trade reporting

    // --- Strategy parameters (set from strategy() declaration) ---
    double initial_capital_ = 1000000.0;
    // Detached on bare native construction. Only the explicit Pine frontend
    // attachment can select its source-shape priority interpretation.
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
    // Account-currency FX multiplier for every quote->account money path. When a
    // strategy declares ``currency=currency.XXX`` differing from the symbol's
    // quote currency (e.g. currency.INR on a USDT-quoted perp), TradingView
    // denominates equity in the account currency but the position notional in
    // the quote currency, converting the latter via the account-currency FX
    // rate before the ``required_margin <= equity`` check. The engine otherwise
    // assumes account == quote (FX 1.0). Injected via the syminfo metadata
    // channel (key "account_currency_fx"); defaults to 1.0 so every corpus
    // strategy (which never sets it) is byte-identical. A timestamped provider
    // may override it as bars advance; the configured scalar remains the
    // fallback before the provider's first effective point and across reruns.
    double account_currency_fx_ = 1.0;
    std::vector<int64_t> account_currency_fx_timestamps_;
    std::vector<double> account_currency_fx_rates_;
    // Per-run broker clock for timestamped FX.  The epoch is the number of
    // provider points effective at the current script-bar open (0 means the
    // scalar fallback).  Consuming an epoch even while flat prevents a later
    // entry from being mistaken for a carried position when the rate has not
    // changed again.
    bool account_currency_fx_broker_epoch_initialized_ = false;
    std::size_t account_currency_fx_broker_epoch_ = 0;
    double account_currency_fx_broker_rate_ = 1.0;

    // TradingView force-liquidation (margin call) toggle. TV runs the broker
    // margin-call emulator by default, so this defaults ON to match TV. It is
    // a no-op for the validation corpus (long-only positions at the default
    // 100% margin can never be liquidated — the formula denominator
    // ``margin/100 - direction`` is 0 — and no corpus short is sized at full
    // equity), and can be switched off via ``set_margin_call_enabled`` for
    // callers that want the legacy hold-to-infinity behaviour.
    bool margin_call_enabled_ = true;

    int64_t trade_start_time_ = std::numeric_limits<int64_t>::min();


    // --- SymInfo + Input injection ---
    SymInfo syminfo_;
    int64_t last_bar_time_ = 0;
    int last_bar_index_ = 0;
    // Live-runtime tail semantics (spec §3.1, ABI v4): when true, the LAST
    // bar of the fed array is a still-forming bar, not the chart's rightmost
    // historical bar. See set_realtime_tail() and apply_realtime_tail_horizon().
    bool realtime_tail_ = false;
    int realtime_tail_horizon_bars_ = 0;
    // Live probe tail suppression (spec §3.2, ABI v4): when true, the LAST
    // array bar (is_tail_bar_) runs only dispatch_bar()'s pre-on_bar broker
    // steps and returns — the run's last-bar fills are the settled book's
    // fills against the forming bar, and the post-run book is the in-force
    // book. See set_probe_suppress_tail_logic(). Independent of
    // realtime_tail_ (do not couple them).
    bool probe_suppress_tail_logic_ = false;
    // Forced intrabar path order (ABI v4 live-runtime surface, task 4): 0
    // AUTO, 1 HIGH_FIRST, 2 LOW_FIRST. Any other value is clamped to AUTO by
    // set_path_order() -- this member is always one of {0,1,2}. Persistent
    // configuration, like realtime_tail_ / probe_suppress_tail_logic_ above
    // -- reset_run_state() does not touch it. See set_path_order() and the
    // legacy PathOrderScope guard in engine_run.cpp. Native-bound source
    // hosts project it into NativeRunSpec::path_order at begin, so the native
    // driver owns the active batch/stream path order.
    int path_order_mode_ = 0;
    // True while dispatching the last array bar (the three run loops set
    // this right after bar_index_ = i). Read by dispatch_bar() to decide
    // whether to apply probe_suppress_tail_logic_. In
    // run_aggregation_bar_loop this is keyed off the INPUT index i, not the
    // emitted script-bar count -- see set_probe_suppress_tail_logic() for
    // why that is only a placeholder today.
    bool is_tail_bar_ = false;
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


    // syminfo.* fundamental/exchange metadata that has no OHLCV source.
    // Returns the value injected via ``set_syminfo_metadata`` for ``key``,
    // or na<double>() when none was injected. Codegen routes the
    // na-by-default SYMINFO_MEMBER_MAP double fields here.
    double get_syminfo_metadata(const std::string& key) const {
        auto it = syminfo_metadata_.find(key);
        return it != syminfo_metadata_.end() ? it->second : na<double>();
    }

    // --- Runtime state ---
    Bar current_bar_;
    int bar_index_ = 0;
    // Opt-in KI-55 chart warmup parity (see set_syminfo_metadata,
    // "chart_ema_na_warmup"). When enabled, chart-timeframe ta.ema instances
    // first used by on_bar na-warm per TV built-in semantics. This selector is
    // scoped independently from request.security so the two execution contexts
    // cannot leak their warmup mode into each other. Default OFF.
    bool chart_ema_na_warmup_ = false;
    // Independent opt-in KI-55 HTF warmup parity. When enabled,
    // request.security series aggregate from security_range_start_ms_ instead
    // of the feed start and their embedded ta.ema na-warm per TV built-in
    // semantics. The cut is taken per evaluator on HTF-BUCKET opens, not on
    // input timestamps: an input bar is dropped when the D/W/M (or intraday
    // grid) bucket it belongs to opened before the range start, so the first
    // HTF bar every series sees is a whole bucket that opened at/after the
    // range start (security_input_precedes_range_start). Default OFF;
    // consulted only by feed_security_eval_state and the historical
    // lookahead projection builder.
    bool security_range_start_na_warmup_ = false;
    int64_t security_range_start_ms_ = 0;
    // The run's first chart bar (0 outside a run). Without the flag above
    // this is the default cut for every coarser-than-chart and chart-
    // timeframe request.security aggregation: TradingView's deep-backtest
    // series of a timeframe hold the bars of that timeframe whose OPEN lies
    // at or after the range start, and the bucket in progress at the range
    // start is absent -- on every lane (round 8, family P: masayanfx
    // multi-time-score; lab tv famp-sense-{es15full,nq15full,f15full,
    // nifty15full,nifty1d,xau1d,xau15,eur15,eth15,btc1d}, 2026-09-05: "D"
    // on CME_MINI:ES1! 15m first reads on the 05-01 20:45Z bar (bucket 0 is
    // the 04-02 trade date; the 04-01 date opened 03-31 22:00Z before the
    // 04-01 00:00Z range start), "240" on 04-04 13:45Z (the 22:00Z bucket
    // dropped), "W" on the 08-29 / 08-28 bar on every 15m and 1D lane (the
    // Mon 03-31 week dropped), while "60" on a 00:00Z start and "D" on the
    // NYSE / NSE lanes (the chart's first bar IS the session open) keep
    // their first bucket). Whether the bucket was in progress is read from
    // the auxiliary 1m feed (did it trade between the bucket's nominal open
    // and the first chart bar? the NSE week whose Monday was a holiday opens
    // on Tuesday and is kept). Historical intraday single-feed forex/cfd D
    // requests also omit their partial first session, using its actual trading
    // open rather than its label. Other single-feed series keep their feed-start
    // behavior; native feeds retain their own rules. Lower-TF evaluators are
    // untouched (their slices begin at the first chart bar anyway), and the
    // flag above keeps its explicit epoch plus the EMA na-warmup semantics.
    int64_t security_first_chart_bar_ms_ = 0;
    // Timestamp of the input bar that FOLLOWS the one being fed to the
    // request.security evaluators; 0 = unknown (streams, the feed's last
    // bar). A historical run holds its whole feed, and the calendar
    // aggregator uses the hint to finalize a D/W/M bucket on the period's
    // actual last chart bar -- early closes and exchange holidays included
    // (TimeframeAggregator::feed(bar, next_input_ms)). Set by the run loops
    // per input bar, per auxiliary bar on the split-feed path, per sub-bar
    // under the magnifier; never by the stream path.
    int64_t security_next_input_ms_ = 0;
    // Nominal close (TradingView's time_close) of the CALLING chart bar the
    // input bar being fed belongs to; 0 = the input bar is the chart bar
    // (single-feed runs, streams). Set per native chart bar on the
    // split-feed path, where a finer auxiliary slice advances
    // request.security under a D/W/M chart bar whose close an OTC
    // calendar bucket compares against the period's nominal close
    // (TimeframeAggregator::feed(bar, next_input_ms, calling_close_ms)).
    int64_t security_calling_close_ms_ = 0;
    // Opt-in historical-only request.security lookahead projection. TradingView
    // can merge a completed higher-timeframe bar onto the first chart child
    // when a finite historical batch is already known. The normal engine path
    // remains progressive, and stream warmup/realtime deliberately ignore this
    // selector so future data can never leak into a live continuation.
    bool historical_security_lookahead_projection_ = false;
    bool historical_security_lookahead_projection_active_ = false;
    uint64_t next_order_incarnation_ = 1;
    // TV: at most one priced ENTRY "open" event per bar; persists across
    // multiple native matching calls (bar magnifier) and dual-pass
    // opposing-stop resolution (see NativeExecutionConsumer).

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
    std::vector<Trade> trades_;
    // TradingView's range-end accounting (record_range_end_close_trades,
    // engine_orders.cpp): the rows that close a position still open after
    // the final script bar, at that bar's close. Report-only — they are
    // merged behind trades_ by fill_trades_section and never enter trades_,
    // the realized sums, or the live position (a stream continues it).
    std::vector<Trade> range_end_trades_;
    // A rejected strategy.entry call leaves no request record behind. The exact
    // collision gate can consume only the immediately preceding source bar, so
    // one scalar tombstone is sufficient and cannot grow with feed length.

    // Actual command/review/sizing causes; policy history is a transient fold.
    // Evaluation-scoped tombstones for live priced ENTRY objects actually
    // removed by strategy.cancel(id). invoke_chart_on_bar clears the map
    // before each script execution; the first fresh same-id strategy.entry
    // consumes the unique cancelled incarnation.

    // strategy.exit partial orders are one-shot per open position for a given id

    // Reusable scratchpad for the per-call opposing-stop deferral set in
    // request matching. Holds the ids of flat-issued entry stops that
    // lost the intra-bar path race in pass 0 and are reconsidered in pass 1.
    // Cleared at the start of each request matching call; the retained
    // capacity avoids a fresh heap allocation 2-4x per bar. Typically tiny
    // (0-1 entries). Not state — must be empty across calls.

    // Reusable scratch for request matching (capacity persists across
    // calls, mirroring scratch_skip_ids_). Incarnations survive OCA erasure;
    // vector indices and retained replacement priorities do not identify an
    // object. Always cleared before use; never persistent cancellation state.

    // Per-PASS dual-entry-stop arbitration winner (a flat position resting
    // one long stop-only ENTRY + one short stop-only ENTRY, both touched
    // this bar -- dual_entry_stop_path_winner, engine_path_resolve.cpp).
    // Reset to None at the top of every request matching CALL (a
    // close-timing mode script bar calls it twice per bar -- old-
    // order settlement, then new-order fills -- and each pass re-derives
    // its own flat-position winner) and written where that arbitration is
    // decided. This is working state, NOT the public accessor's value --
    // it goes back to None the moment the winning side fills (position no
    // longer FLAT) or its admission is declined (the release at the
    // `path_winner_stop_margin_decline` site below), even though a real
    // arbitration happened this bar. last_bar_dual_entry_path() reads
    // last_bar_dual_entry_decision_ (below) instead, precisely to survive
    // that. Only the standard (non-calc_on_order_fills) dispatch path
    // updates this; the COOF scheduler's process_next_pending_order keeps
    // its own unrelated local of the same computation and does not persist
    // it here.
    // Per-BAR snapshot of the above: the last non-None value
    // dual_entry_path_ took during this bar, surviving whatever
    // dual_entry_path_ itself does afterward (a fill, a declined admission
    // release, or the next request matching call's reset). Reset to
    // None once per bar -- at the top of dispatch_bar() and, for the bar
    // magnifier (which never reaches dispatch_bar), where bar_index_
    // advances for each emitted script bar in run_aggregation_bar_loop --
    // and written ONLY alongside dual_entry_path_'s own arbitration write
    // (the native applied-event projection), never at the declined-admission release. ABI v4
    // live-runtime surface (task 4): this is what last_bar_dual_entry_path()
    // returns, so a live probe (or an ordinary POOC run, tail-suppressed or
    // not) reads the bar's real arbitration even if the winning order later
    // filled, was declined, or the working state otherwise moved on. Same
    // fill-recalculation mode caveat as dual_entry_path_ above.

    // --- Trailing stop state ---
    // Best favorable price since position entry (for trailing stop computation)
    // The script bar on which a strategy.exit re-issue restarted
    // trail_best_price_ from the bar's CLOSE under process_orders_on_close
    // (round 9 family Z's restart rule, round 10 family Y's bar rule). The
    // restarted extreme is the NEW order's, and that order's path starts at
    // the next bar's open: the same bar's high/low must not be folded into
    // it by the close-time request matching that follows the script
    // body (update_trail_best_for_bar_open skips this bar). -1 = none.
    // The position's running extreme as it stood BEFORE the current bar's
    // high / low were folded in (update_trail_best_for_bar_open), and the
    // bar it was captured on: a trail leg killed by a declined reversal on
    // this bar restarts from it (round 10 family AE,
    // adapter placement fact `dormant_trail_best`).
    // The ordinary POOC close scan may revisit a retained trail with that
    // same pre-bar extreme only while the carried position is unchanged.
    // A new cycle, add, reduction or close-time trail restart keeps its own
    // established path state instead of inheriting an earlier position's.

    // Best favorable price is updated by the physical open/add helpers for
    // every host. Source policy may consume the same value through
    // inheritance, while the public C observer uses the virtual projection.
    double trail_best_price_ = std::numeric_limits<double>::quiet_NaN();

    // Generic synchronous close obligation. Pine quota/cause/beneficiary
    // state remains exclusively in the compatibility facade above.
    broker::PositionCloseObligation position_close_obligation_;

    // Temporary legacy adapter: only value facts cross into Pine policy.

    // --- Cached trade metrics (updated incrementally in execute_market_exit) ---
    double net_profit_sum_ = 0.0;
    // Conservative absolute roundoff accumulated by additions to the cached
    // net-profit sum. This is numerical provenance only: reported PnL and
    // equity continue to use the unchanged sum above. Infinity means a
    // narrower margin comparison cannot be justified from this history.
    double net_profit_roundoff_bound_ = 0.0;
    // The net value whose additions this bound tracked. Direct/synthetic or
    // future restore writes without matching provenance cannot narrow the
    // established margin comparison; the next trade makes them unbounded.
    double net_profit_roundoff_value_ = 0.0;
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

    // --- Per-script-bar broker-state hash recording (ABI v4 live-runtime
    // surface, task 6). Default off: empty vector, empty report array,
    // every historical run byte-identical to before this flag existed. See
    // set_broker_state_hash_recording() and broker_state_hash(). ---
    bool broker_state_hash_recording_ = false;
    std::vector<uint64_t> broker_state_hashes_;

    // --- Position-size extremes (strategy.max_contracts_held_*) ---
    double max_contracts_held_all_ = 0.0;
    double max_contracts_held_long_ = 0.0;
    double max_contracts_held_short_ = 0.0;

    // --- Even-trade counter (strategy.eventrades) ---
    int eventrades_count_ = 0;

    // --- Risk management (strategy.risk.*) ---

    // Risk state tracking

    // TradingView's strategy.risk.max_intraday_loss (round 7 family M
    // mechanism 5b, pinned 2026-09-05 by lab tv m45-risk-t1/t6/t9/t3b on
    // BINANCE:BTCUSDT 1D and the JOAT probe's threshold ladder
    // m45-joat-risk-*): the rule keeps the DAY-START equity E_ds = realized
    // + the open position marked at the chart-tz day's first tick, and at
    // every emulated tick compares loss = E_ds - (realized + open P&L at
    // the tick) with pct% of E_ds (percent_of_equity) or the absolute
    // value. A fill that CLOSES position quantity is checked with the
    // position already gone but its own realized P&L NOT yet booked, so a
    // profitable exit whose day-start open profit was >= the threshold
    // fires (the JOAT 2026-02-06 short: open profit 2513.6 = 2.452% of
    // 102513.6 fires at 2.45, not at 2.46; the +2699 exit is a gain by every
    // other measure). Realized P&L booked earlier in the day counts at later
    // ticks (t9). Firing closes the position at the tick as "Close Position
    // (Max intraday Loss)", cancels every pending order and blocks every
    // order placement until the chart-tz day changes (t1: the close-calc
    // order of the fired day is dropped, the next day's fills). The rule
    // never latches risk_halted_ (that stays with max_drawdown /
    // max_cons_loss_days).
    // A fire inside a fill loop defers the pending-order cancel to the
    // loop's safe point (finish_intraday_loss_cancel); the loop itself
    // removes every order it has not yet applied.
    // @broker-state end
    // Host-installed excursion capability (RULING A48).  Transient run
    // wiring, not durable broker state: reset_run_state clears it and the
    // consumer reinstalls it once per run when the host declares ownership.
    LotExcursionHook lot_excursion_hook_;
    // Continuation digest at the last script point. Native batch teardown
    // moves the consumer into Completed and would otherwise change the scalar
    // relative to the recorded array; source state is still folded live so
    // post-run mutations remain visible.
    uint64_t last_script_continuation_hash_ = 0;
    bool last_script_continuation_valid_ = false;
    // --- Per-trade extreme tracking ---
    execution::Result settle_native_execution_at(
        const execution::Action& action, const execution::Fill& fill,
        const execution::PhysicalExecutionContext& context);
    execution::Result settle_with_context(
        const execution::Action& action, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle,
        const execution::PhysicalExecutionContext& context);
    execution::SettlementInspection inspect_native_settlement(
        const execution::Action& action, const execution::Fill& fill) const;
    // Synchronous selected-close extensions. Book preserves the original
    // action semantics; OpeningExposure permits only Flatten/Reduce and is
    // revalidated against the current physical book on every call.
    execution::SettlementInspection inspect_native_settlement_scoped(
        const execution::Action& action, const execution::Fill& fill,
        execution::CloseScope scope) const;
    execution::Result settle_native_execution_scoped_at(
        const execution::Action& action, const execution::Fill& fill,
        const execution::PhysicalExecutionContext& context,
        execution::CloseScope scope);
    // --- Additive selected close (not a CloseScope overload) ---
    execution::SettlementInspection inspect_native_settlement_selected(
        const execution::Action& action,
        const execution::Fill& fill,
        const execution::SelectedOpeningSet& selection) const;
    execution::Result settle_native_execution_selected_at(
        const execution::Action& action,
        const execution::Fill& fill,
        const execution::PhysicalExecutionContext& context,
        const execution::SelectedOpeningSet& selection);
    // Source lifecycle coordinator: current_bar_/fold context, source-day
    // preflight, selected commit, then observation of its committed close rows.
    // Empty lifecycle is valid. Do not fold selection into LifecycleEffects.
    execution::Result settle_execution_selected_with_lifecycle(
        const execution::Action& action,
        const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle,
        const execution::SelectedOpeningSet& selection);
    // --- Additive non-applying account/effect projection ---
    execution::AccountEffectProjection project_native_settlement_v1(
        const execution::Action& action,
        const execution::Fill& fill) const;
    execution::AccountEffectProjection project_native_settlement_scoped_v1(
        const execution::Action& action,
        const execution::Fill& fill,
        execution::CloseScope scope) const;
    execution::AccountEffectProjection project_native_settlement_selected_v1(
        const execution::Action& action,
        const execution::Fill& fill,
        const execution::SelectedOpeningSet& selection) const;
    // Opposite-book reversal to an exact signed exposure. Unlike Transact,
    // the opening is not a remainder of transaction units minus held units.
    // These synchronous extensions share the settlement owner and do not
    // introduce a queued native request or saved commit authority.
    execution::SettlementInspection inspect_native_reversal_v1(
        const execution::ReverseTo& reversal,
        const execution::Fill& fill) const;
    execution::AccountEffectProjection project_native_reversal_v1(
        const execution::ReverseTo& reversal,
        const execution::Fill& fill) const;
    execution::Result settle_native_reversal_at_v1(
        const execution::ReverseTo& reversal,
        const execution::Fill& fill,
        const execution::PhysicalExecutionContext& context);
    // Source reversal coordinator with current chart context and source days.
    execution::Result settle_reversal_with_lifecycle_v1(
        const execution::ReverseTo& reversal,
        const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle);
    // Native account value: realized balance plus marked physical lots minus
    // their remaining paid entry costs, for every fee type. No Pine sizing or
    // end-of-range reporting convention participates in this value.
    double marked_equity(double price) const;

    // --- Strategy order commands ---
    // NOTE: prior to v0.2 the runtime accepted a leading `double market_price`
    // positional after `is_long`. The implementation never read it; every
    // fill price came from `current_bar_.close` inside the function body,
    // and every closed-transpiler call site passed `current_bar_.close`
    // verbatim. Parameter dropped to match TradingView's `strategy.entry()`
    // surface. Consumer codegen must be regenerated alongside this commit.


    // Keep the historical five-argument symbol above; regenerated legacy
    // sources continue to bind token 0. This does not promise that arbitrary
    // objects compiled against an older BacktestEngine class layout can be
    // relinked without rebuilding. New codegen supplies a stable nonzero token
    // for the syntactic strategy.close source site.









    // TradingView forced-liquidation (margin call). Finite-price liquidation
    // paths use the bar's adverse extreme. A 100%-margin long instead uses
    // opening affordability and the separately scoped rounded-money checks.

    // Settle an opening money restore before a later eligible owned exit,
    // retaining the actual chart bar for financial-class eligibility.

    // Ordinary subcontract shorts and integer MARKET lots expose completed
    // liquidation to the close-time script (R23/R25/R28 TV controls).

    // An unchanged carried POOC short finishes its adverse-path margin event
    // before the close script observes or reverses it. The caller proves no
    // resting order filled earlier on this bar.

    // finding-308: chronological pre-exit forced-liquidation slice. Called
    // from the request matching fill loop immediately BEFORE a priced
    // exit of the live position is applied. Fires only when (a) no margin
    // call was booked on this bar yet, (b) the bar's adverse extreme comes
    // STRICTLY earlier on the synthesized intrabar path than the exit's
    // fill (a tie — the exit filling exactly at the extreme — keeps the
    // exit first), and (c) the pre-fill position is already in margin
    // deficit at that extreme. The slice mirrors the adverse-cascade
    // trigger/slice arithmetic of process_margin_call byte-for-byte.
    // Returns true when a "Margin call" row was booked; the triggering exit
    // then fills the reduced remainder.

    // finding-325 (1x-long entry-fill affordability chronology): TV runs the
    // 1x-long (margin_long=100) opening-affordability check AT THE ENTRY
    // FILL, chronologically before the same bar's intrabar exits. When a
    // priced exit of a just-opened 1x long is about to fill on the entry's
    // own bar and the floor-sized opening cost exceeds post-close equity,
    // the one-shot opening event books its trim FIRST — the ordinary
    // floor-before-4x quantity (including the sub-lot one-contract
    // fallback), filled at the RAW matched entry base, tagged "Margin call"
    // — and the exit then closes the reduced remainder. Consumes the
    // pending opening event; returns true when a slice was booked.

    // A timestamped FX rollover is a broker-open event, not an end-of-bar
    // adverse-price check.  Cell A1 supports carried 1x full-margin long and
    // short in ordinary historical dispatch; leveraged shapes stay fail-closed.
    // Returns true when it emits a broker liquidation row.

    // finding-430: forced liquidation at the bar OPEN. A carried position
    // with a finite liquidation price that already breaches the margin
    // requirement at the open is sliced AT THE OPEN (quantity computed at
    // the open price), before any resting order is evaluated there; the
    // survivor keeps its ordinary adverse-extreme check, so one bar can book
    // an open slice AND an extreme slice. Bars whose open does not breach
    // are untouched. Returns true when a "Margin call" row was booked.

    // Round 7 family H residual (NYSE:F 1D short admission tape 2025-04-23 /
    // 2026-04-08): a strategy.close / close_all MARKET order for the WHOLE
    // position resting for this bar's open fills before the open's margin
    // evaluation — TradingView closes 1025 @9.84 through the pending close
    // where the engine sliced 48 @9.84 first. True when such an order rests
    // in the book (created on a prior bar, no priced leg, covers the whole
    // position) AND no opposite-side entry order rests for the same open:
    // a close paired with a reversal entry is voided when TradingView
    // declines that reversal by admission (pin log-20260905t111645z-
    // e1783b94), so the open slice must stand (round-8 regression on the
    // all-in reversal scripts: amandaborgeson06 F@15 2025-05-01 13:30Z,
    // hexatrades AAPL@15 2025-07-29 13:30Z). margin_call_slice_at_bar_open
    // then stands down only for the unconditional close. A partial close or
    // a priced exit the open gapped through keeps the open slice (unpinned).

    // Round 7 family L (campaign pin log-20260905t093952z-0c4938cb; lab tv
    // tapes scratchpad/r7/pins/xau15-mcpath-{a,b}, fresh-touch-once): on the
    // bar a position OPENS, TradingView marks the forced liquidation only
    // over the part of the synthesized O-H-L-C / O-L-H-C path AFTER the
    // entry fill — a bearish bar whose stop fill lies below the open never
    // sees that bar's high (asian-box 04-01 15:45Z: no slice; xau15-mcpath-a:
    // the slice comes on the next bar at its high), the bar CLOSE is a mark
    // point (fresh-touch-once: 8 @11.25 = the entry bar's close), and a fill
    // at the open (market, or a stop the open gapped through) sees the whole
    // bar (xau15-mcpath-b, mdfe3757 04-08 13:30Z: same-bar slice at the
    // high). Carried bars keep the whole-bar extreme.
    //
    // True when the just-opened position leaves a path suffix on `bar`:
    // *out_mark is the suffix's adverse extreme (RAW price — the waypoints
    // after the fill, the close included; the caller applies the cascade's
    // own mintick rounding), *out_pos its path position (waypoint index, in
    // first_touch_position's units). An unrouted fill coordinate (market /
    // open fills, stop-limit, raw orders) reads as the open, i.e. the whole
    // bar as before; a fill at the close has no suffix (false).

    // The dispatch shapes the entry-bar path rule is pinned for: the position
    // opened on this bar under ordinary historical dispatch (no
    // process_orders_on_close, calc_on_order_fills, bar magnifier or
    // streaming). Everything else keeps the whole-bar extreme.


    // --- Fill rounding helpers ---
    // Nearest-tick rounding: TradingView's exact double-precision function
    // floor(price / mintick + 0.5) * mintick, with NO epsilon.
    //
    // finding-446: TV's own NASDAQ:AAPL and OANDA:EURUSD series carry
    // sub-tick prints (x.xx5 and a few 4-dp values). Every TV fill taken at
    // one of those RAW BAR PRICES is exactly this function of it: 24,582 /
    // 24,582 half-cent AAPL closes (22,122 rounded up, 2,460 rounded DOWN
    // because the binary quotient lands just under the midpoint —
    // 228.765 / 0.01 = 22876.499999999996 -> 228.76, while
    // 214.385 / 0.01 = 21438.5 -> 214.39) and 142,938 / 142,938 EURUSD
    // fills at tick 1e-5. Any epsilon nudge (floor(r + 0.5 + 1e-6)) or a
    // decimal half-up rule forces every binary midpoint up and breaks the
    // 2,460 down cases. For r >= 1 the form below is bit-identical to
    // std::round(r) (adding 0.5 is exact inside one binade), so the legacy
    // std::round shape was already the right function; it is spelled out so
    // the code reads as the census formula it was fitted to.
    double round_to_mintick(double price) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        return std::floor(price / syminfo_mintick_ + 0.5) * syminfo_mintick_;
    }

    // round 8 family R scope: the 10-significant-digit money arithmetic
    // (tv_money_round) is applied where it was pinned and where it can move a
    // lot — a lot-stepped instrument whose lot is worth less than one unit of
    // account currency at the sizing price (OANDA:EURUSD 0.01 x 1.1 = 0.011;
    // BINANCE:BTCUSDT 1e-5 x 85,000 = 0.85; ETHUSDT.P 1e-4 x 2,500 = 0.25).
    // On integer-lot instruments (F, AAPL, ES, NQ, NIFTY: a lot is 10..250k)
    // and on the corpus' continuous qty_step 0 the exact arithmetic stays:
    // there the rounding could only ever bite on a synthetic exact tie.
    // POOC flat-parent controls: cost rounding also precedes admission at the
    // terminal close. Named child brackets created after the sole pending
    // parent have no live owner yet; they cannot compete for its opening cash.
    // Same-bar POOC money admission for one true-flat long MARKET parent.
    // Quantity sizing keeps its existing slipped divisor. The money checks
    // use the recorded signal mark and a tick-built slipped price instead.

    // R24 signal-cost controls: a fractional lot worth >=1 account unit can
    // still cross the rounded-money admission boundary. BTC Q9.36259 at
    // 112380.33 costs 1052170.9538547, rounded to 1052170.954; exact signal
    // equity 1052170.9536054998 therefore keeps only a reversal's close leg.
    // XAU Q300 at 3443.625 likewise rejects capital cost-0.0001, while exact
    // cost and cost+0.0001 admit, despite the cheaper next opening price.
    // R39 BTC/XAU controls also pin the independent price-scale check for
    // this ordinary market book. Share its scope while keeping the separately
    // pinned POOC signal-cost extension out of the price-scale extension.
    // Rule 1's scope (round 10 family AE): the ten-digit equity and the raw
    // lot floor size EVERY lot-stepped instrument — integer shares included
    // (NASDAQ:AAPL 2025-09-05 16:15Z: 1094521.68 / 238.77 floors to 4583 on
    // TradingView, 4584 from the float-accumulated ledger or a nudged
    // floor). Only a continuous qty_step 0 keeps the exact arithmetic.
    // The broker's required margin at a mark is money at ten significant
    // digits where that can move a lot (famr-adm-rev-01000: the 4x trim of
    // the fill bar's short is 3900.60 from the ROUNDED 1000527.321 required
    // margin, 3900.56 from the exact 1000527.3207023 — one lot of restore
    // quantity across the 0.01 floor; rev-00800 / -01079 / revb / S3 keep
    // their trims either way). Exact outside the scope.

    // A fill taken AT A RAW BAR PRICE — a market order at the bar close
    // (process_orders_on_close) or at the next open, a resting stop/limit
    // the open gapped through, a stop-limit whose limit is already
    // marketable at an OHLC path point, a margin-call slice at the open or
    // adverse extreme, a strategy.close at the close / COOF bar-point
    // cursor — books the raw print rounded to the NEAREST tick and only then
    // carries slippage ticks. The FEED is never quantized (indicators consume
    // the raw sub-tick values); only the fill and the broker's default-sizing
    // snapshot (calc_qty / frozen_sizing_price, same nearest-tick form) are
    // on-tick. The directional
    // snap (round_to_mintick_directional) is reserved for COMPUTED stop /
    // limit LEVELS that fall between ticks; applying it to a raw bar price
    // was the finding-432/446 defect (sells floored, buys ceiled — 43 AAPL
    // slugs off by one tick). The result is on-tick, so the directional snap
    // downstream in apply_slippage is an identity on it (the 1e-9 boundary
    // guard absorbs the n*tick/tick FP residue).
    double bar_fill_price(double raw_bar_price) const {
        return round_to_mintick(raw_bar_price);
    }

    // design-stop-tick-rounding (round 6): the broker emulator TESTS a
    // resting stop / limit against the bar's OHLC quantized to the tick
    // (nearest, the finding-446 formula), while the order LEVEL stays raw;
    // the fill keeps its existing directional / limit-or-better snap.
    //
    // Pinned on NYSE:F 1D (mintick 0.01, sub-penny prints; lab tv tapes
    // scratchpad/r6/pins/stopround-*, 2026-09-04):
    //   long sell-stops 13.74624 / 13.7451 / 13.7449 / 13.745 all SKIP
    //     2026-02-02 (low 13.745 -> 13.75) and fill 02-03 @13.74, while
    //     13.3449 fills 01-26 (low 13.3448 -> 13.34) @13.34 — neither a raw
    //     compare (02-02 would fill) nor a floored/ceiled level (01-26
    //     would not) explains both; only the quantized low does;
    //   short buy-stops 14.0349 / 14.03505 / 14.0352 all fill 02-03 (high
    //     14.0351 -> 14.04) @14.04; 13.225 skips 12-09 (high 13.2202 ->
    //     13.22): the high rounds to NEAREST, not up;
    //   sell-stop 13.776 over the 02-20 open 13.775 (-> 13.78) fills at the
    //     level 13.77, not at the open: the open is quantized too;
    //   the same bars/levels reproduce for strategy.exit(limit=),
    //     strategy.entry(stop=) / (limit=) and strategy.order(stop=), long
    //     and short (stopround-xl-*, -es-*, -el-*, -eo-*);
    //   stopround-ohlc-0/1 encode Pine's own low/high in the trade qty:
    //     13.745, 13.3448, 14.0351 — the RAW prints, identical to the feed,
    //     so the quantization lives in the broker, not the data.
    // The trail leg is NOT covered (stopround-xt-L-trail: trail_points 20 /
    // trail_offset 3 over the 14.035 high exits at the next open, the raw-
    // extreme behaviour the engine already has), so the trail keeps walking
    // the raw path; stop-limit entries, the process_orders_on_close close
    // compares and the calc_on_order_fills cursors were not pinned either
    // and stay raw.
    //
    // The grid point is materialized as k / (1/mintick) when 1/mintick is
    // integral (every decimal tick), which is the double a Pine literal on
    // that tick parses to — so an on-grid level compares EQUAL to a
    // quantized bar price bit-for-bit (14.04 vs k*0.01 = 14.040000000000001
    // would not). Non-decimal ticks fall back to k*mintick.
    double tick_grid_price(double price) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        const double k = std::floor(price / syminfo_mintick_ + 0.5);
        const double inv = 1.0 / syminfo_mintick_;
        const double inv_int = std::floor(inv + 0.5);
        if (inv_int > 0.0 && std::abs(inv - inv_int) <= 1e-6 * inv_int) {
            return k / inv_int;
        }
        return k * syminfo_mintick_;
    }
    // round 8 family T (NYSE:F@15, pinned by 40 `lab tv` sensor tapes on
    // NYSE:F / CME_MINI:ES1! / OANDA:EURUSD 15m, scratchpad famT/pins,
    // 2026-09-05): TradingView stores a resting stop / limit LEVEL on the
    // symbol's PRICE GRID (multiples of 1 / pricescale, pricescale =
    // 10^decimals of the tick) whenever the level sits within
    // 0.01 / pricescale^2 of a grid price — the residue a level computed
    // as avg_price +/- k * mintick carries (9.99 + 0.05 =
    // 10.040000000000001, 11.86 - 0.05 = 11.809999999999999) and anything
    // up to 1e-6 on a 2-decimal symbol (10.040001 IS 10.04; 10.0400012 is
    // not and takes the directional snap: a sell limit at 10.0400012 fills
    // at 10.05). ES1! (tick 0.25, pricescale 100) has the same 1e-6 band
    // (5513.7500005 IS 5513.75, 5513.750001 is not); EURUSD (pricescale
    // 1e5) snaps only within 1e-12 (1.135560000001 IS 1.13556,
    // 1.135560000002 is not). The engine used to compare the RAW level
    // against the tick-quantized bar, so a bar whose quantized extreme
    // EQUALS the level (h 10.04, or h 10.035 -> 10.04, vs
    // 10.040000000000001) did not fill and the exit landed bars later at
    // the same snapped price — 148 of the 179 exit-time mismatches on the
    // F@15 lane (masayanfx-scalping 102, latibonit 17, jos-protrader 8,
    // vasudevshenoy 6, lukeborgerding, drakkhon, rhyme17, hariss369,
    // colasbreugnon, fast-scalper, JOAT aureate). Applied where a level is
    // stored on a request record (strategy.entry / exit / order, and the
    // profit / loss tick conversion), so every trigger test, gap test,
    // marketable-at-placement test and fill snap reads the grid value —
    // materialized as k / pricescale, the double the decimal literal
    // parses to, bit-for-bit equal to tick_grid_price's output. A level
    // outside the band is returned untouched (the directional fill snap
    // and the exact trigger compare keep TV's sub-tick behaviour, round 6).
    // A binary tick (1/128 = 0.0078125) resolves to a 7-decimal grid whose
    // band is 1e-16: effectively untouched; a tick with no short decimal
    // expansion has no grid and is untouched.
    static constexpr double kLevelGridBandPoints = 0.01;  // x 1/pricescale
    int price_grid_decimals() const {
        if (syminfo_mintick_ <= 0.0) return -1;
        double scaled = syminfo_mintick_;
        for (int n = 0; n <= 10; ++n) {
            const double k = std::floor(scaled + 0.5);
            if (k >= 1.0 && std::abs(scaled - k) <= 1e-6 * k) return n;
            scaled *= 10.0;
        }
        return -1;
    }
    double level_on_price_grid(double level) const {
        if (std::isnan(level) || !std::isfinite(level)) return level;
        const int n = price_grid_decimals();
        if (n < 0) return level;
        double pricescale = 1.0;
        for (int i = 0; i < n; ++i) pricescale *= 10.0;
        // Points as level / pointsize (pointsize = 1 / pricescale as a
        // double): of the candidate arithmetics this is the one that
        // reproduces every tape at the band's FP boundary (EURUSD
        // 1.13556 + 1e-12 snaps, + 2e-12 does not; ES 5513.75 + 5e-7
        // snaps, + 1e-6 does not; F 10.04 + 1e-6 snaps, + 1.2e-6 does not).
        const double pointsize = 1.0 / pricescale;
        const double p = level / pointsize;
        const double k = std::floor(p + 0.5);
        if (std::abs(p - k) <= kLevelGridBandPoints / pricescale) {
            return k / pricescale;
        }
        return level;
    }
    Bar broker_tick_bar(const Bar& bar) const {
        Bar b = bar;
        b.open = tick_grid_price(bar.open);
        b.high = tick_grid_price(bar.high);
        b.low = tick_grid_price(bar.low);
        b.close = tick_grid_price(bar.close);
        return b;
    }
    // The bar the stop / limit trigger tests run on. A synthetic bar — the
    // calc_on_order_fills scheduler's point / monotonic-segment bars and the
    // KI-67 cascade waypoint bar — is a slice of an already-decided path and
    // is compared raw, exactly as before; a real chart / lower-TF bar is
    // quantized.

    // TradingView fills stop entries directionally to mintick rather than
    // rounding to nearest: long stops snap UP (ceil), short stops snap DOWN
    // (floor). Verified against basic/parabolic-asr where the 2,513
    // non-gap stop entry fills show a perfectly one-sided +/-0.01 bias.
    // See investigation report at /tmp/pf_investigation_parabolic_asr.md.
    // This applies to COMPUTED LEVELS only (a stop at (open+high)/2, a
    // user_close + 0.5 level, ...). A fill at a RAW BAR PRICE goes through
    // bar_fill_price (nearest tick, finding-446) before it reaches the
    // slippage path, where this snap is then an identity.
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
        // TradingView snaps LEVEL fills to mintick directionally even when
        // slippage is zero: a buy fills at the next-higher mintick, a sell
        // at the next-lower mintick. (A raw bar price arrives here already
        // nearest-rounded by bar_fill_price, finding-446, so the snap below
        // is an identity on it.) The legacy nearest-mintick rounding biased
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

    // --- Commission helper ---
    // PERCENT commission is a % of the order's notional value. The notional
    // (fill_price × qty × pointvalue) is in the symbol's QUOTE currency; the
    // commission a strategy() reports is in ACCOUNT currency, so it needs the
    // same instrument->account conversion as the margin gate below
    // (account_currency_fx_, default 1.0 — no-op for the corpus). Cash-per-
    // order / cash-per-contract are already account-currency-native (a
    // trader configures "$20 per contract" in their own currency), so they
    // are untouched.
    double calc_commission(double fill_price, double qty) const {
        switch (commission_type_) {
            case CommissionType::PERCENT:
                return fill_price * qty * syminfo_.pointvalue
                       * active_account_currency_fx()
                       * (commission_value_ / 100.0);
            case CommissionType::CASH_PER_ORDER:
                return commission_value_;
            case CommissionType::CASH_PER_CONTRACT:
                return commission_value_ * qty;
        }
        return 0.0;
    }

    // Read the paid entry fee for a still-open pyramid slice. Production entry
    // paths always initialize the snapshot. The fallback keeps hand-constructed
    // PyramidEntry fixtures source-compatible without weakening real lifecycle
    // behavior.
    double open_entry_commission(const PyramidEntry& pe) const {
        return std::isfinite(pe.entry_commission_account)
            ? pe.entry_commission_account
            : calc_commission(pe.price, pe.qty);
    }

    double allocated_entry_commission(const PyramidEntry& pe, double units) const {
        if (units <= 0.0) return 0.0;
        const double paid = open_entry_commission(pe);
        return units >= pe.qty ? paid : paid * (units / pe.qty);
    }

    void snapshot_entry_commission(PyramidEntry& pe) const {
        pe.entry_commission_account = calc_commission(pe.price, pe.qty);
    }

    // --- Position sizing helper ---
    // PERCENT_OF_EQUITY / CASH size a budget that is denominated in ACCOUNT
    // currency (equity, and a strategy.cash default_qty_value are both
    // account-currency-native — see emit_close_trade / current_equity()),
    // then convert it into a quantity of the instrument, whose price is in
    // QUOTE currency. Divide the account-currency cash by account_currency_fx_
    // first (the inverse of the instrument->account multiply used for
    // commission/PnL/margin) so the division by fill_price stays dimensionally
    // consistent; default 1.0 leaves the corpus untouched.
    // Floor an order quantity to the instrument's tradable lot increment
    // (qty_step_). TradingView applies this to EVERY order it sends to the
    // exchange, not just forced liquidations — verified row-for-row: a
    // computed DCA/safety-order quantity (e.g. baseOrderSize/close) is
    // floored, not rounded, before it ever contributes to cost basis or a
    // fill (the source adapter applies the same rule to liquidation lots).
    // qty_step_ == 0 (corpus default) leaves qty
    // untouched. A quotient that is only binary64 residue below an integer is
    // treated as that integer, using the same 1e-6-of-a-step tolerance as
    // percent-derived exits below. This keeps an on-grid request such as
    // 1 / 0.00001 from losing a whole lot because the quotient materializes as
    // 99999.999999..., while a genuinely off-grid request still floors. When
    // quantization would be a no-op, preserve the original double so the
    // tolerance never increases a requested quantity. Unlike the liquidation
    // path, a regular entry legitimately CAN floor to zero (an under-funded
    // order is simply not placed), so there is no "never stall"
    // floor-to-one-step fallback here.
    double apply_qty_step(double qty) const {
        if (qty_step_ <= 0.0 || !std::isfinite(qty) || qty <= 0.0) return qty;
        double floored = std::floor(qty / qty_step_ + 1e-6) * qty_step_;
        return floored < qty ? floored : qty;
    }

    // Percent-derived strategy.exit lots are floored to the same lot
    // increment (TV evidence, BINANCE:ETHUSDT.P qty_step 0.0001: a
    // qty_percent=50/50 short bracket over a 5.4103 position fills
    // 2.7051 + 2.7051, leaving a 0.0001 dust short OPEN until the next
    // reversal/close/margin-call — 39 of stockhunter2025-btcusd-4h-ema-
    // swing-strategy's 56 unmatched TV trades were exactly such dust
    // rows). Unlike apply_qty_step this floor is epsilon-tolerant: 50%
    // of an on-grid position is often exactly on-grid in real numbers
    // but lands one ulp below the grid ratio in doubles (2.7051/0.0001
    // = 27050.999999…), and a plain floor would knock such a leg a FULL
    // step down, inventing dust TV does not have. The tolerance (1e-6 of
    // a step) sits far above double representation error at realistic
    // qty/step magnitudes yet far below any genuine sub-step remainder.
    // When the floor is a no-op (qty already on-grid) the ORIGINAL double
    // is returned unchanged: reconstructing it as floor(...)*step lands
    // one ulp away (0.3 -> 0.30000000000000004) and that representation
    // jitter leaks into printed PnL at the 1e-6 digit for strategies whose
    // percent legs were already exact (officialjackofalltrades' 30%-of-1
    // legs) — a pure artifact this fix must not introduce.
    double apply_exit_qty_step(double qty) const {
        if (qty_step_ <= 0.0 || !std::isfinite(qty) || qty <= 0.0) return qty;
        double floored = std::floor(qty / qty_step_ + 1e-6) * qty_step_;
        return floored < qty ? floored : qty;
    }

    // Integer-lot symbols keep one minimum contract/share for any positive
    // percent-derived strategy.exit request when at least one whole step is
    // still unreserved. TradingView evidence on one-contract ES/NQ/NIFTY
    // positions shows a pair of qty_percent=50 siblings reserving 1 + 0, not
    // 0 + 0. Fractional-lot symbols retain the floor/dust rule above.
    //
    // This helper is intentionally exit-percent-specific: explicit exit qty,
    // full-percent exits, entries and other broker quantities must not acquire
    // a minimum-one-step fallback.
    double apply_percent_exit_qty_step(double requested_qty,
                                       double available_qty) const {
        double gridded = apply_exit_qty_step(requested_qty);
        if (qty_step_ >= 1.0
            && requested_qty > 0.0
            && requested_qty < qty_step_
            && available_qty >= qty_step_) {
            return qty_step_;
        }
        return gridded;
    }

    // The broker's sizing arithmetic runs ON-TICK. Both the price the budget
    // is divided by and the price the open position is marked at for the
    // equity term are round_to_mintick() of their raw inputs; the FEED itself
    // stays raw (ta.*, crossovers, plots and every strategy.* metric TV
    // reports on raw values keep reading current_bar_.close directly — only
    // this sizing snapshot and the fill are quantized). The evidence is the
    // same tape family that pinned the signal-bar freeze below:
    //
    //   - taro-s-c-c-ma-simplified-2-color replayed over the NYSE:F and
    //     NASDAQ:AAPL tapes: qty = floor(E / tick(close_S)) with E marked at
    //     tick(close_S) reproduces 674/674 F and 832/832 AAPL reversals; the
    //     raw-close divisor fits only 476/674 on F. The same replay matches
    //     675/675 TV entries by RAW-close crossovers, which is why the signal
    //     path is left alone.
    //   - drgunjan-F trade 1: a 9.565 signal close, TV qty 10460 =
    //     floor(100000 / 9.56) (9.565 / 0.01 = 956.49999... rounds DOWN under
    //     the census formula, exactly as 228.765 does); the raw divisor gave
    //     floor(100000 / 9.565) = 10454.
    //   - The raw basis also DECLINED entries TV filled: when an x.xx5 close
    //     rounds UP at the fill (bar_fill_price) while the quantity was
    //     floored against the raw close, qty * fill exceeds the sizing
    //     equity by ~qty * mintick/2 and the true-flat gap-reject / reversal
    //     float-guard arms in apply_filled_order_to_state saw a phantom gap.
    //     463/463 missing taro-F entries were predicted by that mechanism
    //     with 0 counterexamples; 26/26 drgunjan-F and 6/6 mazi-F missing
    //     entries had sub-penny signal closes.
    //
    // On a price that is already n*tick (bar_fill_price output, a
    // directionally-snapped level, or the slipped sizing price
    // frozen_sizing_price builds) round_to_mintick is identical to within
    // ONE ULP — not an identity: double(tick) is inexact for every decimal
    // tick, so floor(x/0.01 + 0.5)*0.01 != x for 6,951 of 49,900 decimal-
    // parsed 2dp prices 1.00..500.00 (always +1 ulp, double(0.01) > 0.01),
    // for 35,736 of 70,000 5dp prices at tick 1e-5, and for 0 of 12,000 at
    // the binary-exact 0.25. The ulp is absorbed by apply_qty_step's 1e-6
    // nudge (0 floor flips of floor(100000/x) across all 49,900 prices) and
    // by the 1e-9 / 1e-12 float guards on every admission arm, so no
    // quantity or verdict moves on an on-tick feed: the fill-time legacy
    // callers and the frozen path reproduce the pre-fix numbers there, and
    // only sub-tick prints move — to TV's number.
    //
    // QtyType::CASH follows percent_of_equity by construction (the same
    // broker division, only the numerator differs) and is UNPINNED: every
    // census above is default percent_of_equity sizing, and no strategy.cash
    // tape discriminating round(close) from close has been replayed. A
    // future CASH mismatch on a sub-tick feed is traced here first.

    // TradingView freezes DEFAULT (qty=na) market-order sizing at the SIGNAL
    // bar — the bar whose on_bar issued the strategy.entry/strategy.order
    // call — not at the fill:
    //
    //   tick(x)      = round_to_mintick(x)          // nearest tick, census form
    //   equity_S     = initial_capital + realized net profit
    //                  + open_profit(tick(close(S)))  // position may still be OPEN
    //   sizing_price = tick(close(S)) + slippage*mintick*(+1 buy / -1 sell)
    //   qty          = floor_step( commission_reserved(budget)
    //                              / fx / (sizing_price * pointvalue) )
    //                  // commission_reserved divides by (1 + commRate) for a
    //                  // PERCENT commission and is the identity otherwise;
    //                  // the adapter owns it (docs/pine-adapter-kernel-notes.md)
    //
    // The market order then fills at the NEXT bar's open carrying this frozen
    // quantity. calc_qty(price) implements exactly that shape when evaluated
    // AT SIGNAL TIME (current_bar_ IS the signal bar: open_profit marks at
    // tick(close(S)) and the divisor is the rounded argument), so the freeze
    // is simply calc_qty(slipped rounded signal close) captured at
    // placement. Evaluating the same expression at FILL time — the
    // pre-freeze behavior — was wrong in
    // three separable ways on a reversal/gap: it double-counted the just-
    // closed position's PnL (current_equity() already realized the exit while
    // position_* still held the stale lot for open_profit), it marked open
    // profit at the FILL bar's close (a look-ahead: that close is unknown
    // when the order fills at the open), and it divided by the fill price
    // instead of the signal close. Freezing at placement removes all three.
    //
    // Only PERCENT_OF_EQUITY / CASH default sizing is price/equity-dependent;
    // FIXED default sizing stays qty=NaN at placement (identical value at
    // fill, and keeping NaN preserves the isnan(order.qty)-keyed semantics
    // elsewhere, e.g. the OCA "fully filled" heuristic).
    //
    // Priced (limit/stop) entries are NOT frozen: TV's sizing basis for an
    // order armed one or more bars before its fill is not empirically
    // established, so they conservatively keep the legacy fill-time sizing.
    // The sizing price of the frozen rule above, exposed separately so the
    // placement sites can retain it as an adapter placement fact (`sizing_price`)
    // for the fill-time margin-admission re-check.
    //
    // The basis is the mintick-ROUNDED signal close. Rounding happens BEFORE
    // the slippage ticks are added so the result is n*tick for any feed
    // print, exactly as a bar_fill_price fill carries its slippage: TV's
    // broker never sees the sub-tick close Pine sees (674/674 F, 832/832
    // AAPL reversals on the taro tapes fit tick(close_S); the raw close fits
    // 476/674 — see calc_qty). A feed that is already on-tick is unaffected
    // in every quantity and verdict, though not bit-for-bit: round_to_mintick
    // returns the n*tick double to within one ulp (double(tick) is inexact
    // for decimal ticks — the measurement is in calc_qty's comment), and
    // that ulp is absorbed by apply_qty_step's 1e-6 nudge and by the
    // admission arms' float guards.


    // round 7 (family M, JOAT BTC@1D; campaign note log-20260905t121513z-
    // 50167cb8, CORRECTING the m1d-coof-ctx pin): a DEFAULT-sized
    // (percent_of_equity / cash) MARKET order that a calc_on_order_fills
    // FILL RECALC places is sized by TradingView at ITS OWN FILL, not at the
    // signal bar's close and not at the recalc's cursor:
    //
    //   lab tv scratchpad/pins/m1d-coof-size-btc (BINANCE:BTCUSDT 1D,
    //   2025-10-01..12-31, tv-tape-m1d-coof-size-btc-7ee8712b): "B", born in
    //   the SECOND recalc at the 10-02 open and filled at W1 = the low
    //   118279.31, has qty 845.4564 = 10% x 1e9 / 118279.31 (cursor O
    //   118594.99 -> 843.2; the bar's close 120529.35 -> 829.7); thirteen
    //   entries born in a first-O recalc and filled at O size at O, never at
    //   the finals close. The probe itself: TV 4 0.09245 = 9802.56 /
    //   (106011.13 x 1.0001) at the 11-11 open fill (the engine froze 0.0951
    //   at the 11-11 close 103058.99); TV 10 0.14674 at its W2 fill 69988.83
    //   (cursor W1 63913.27 -> 0.16069, close 67988.04 -> 0.15106).
    //
    // The script context of such a recalc is unchanged — TradingView runs it
    // on the CURRENT bar's finals (scratchpad/pins/m1d-coof-ctx2-{btc,f}:
    // 103/103 encoded firings read bar k's high/low/close/volume, bar_index k,
    // barstate.isconfirmed true, close[1] = bar k-1) — exactly what
    // execute_coof_script_body presents. Only the SIZING moment differs from
    // an ordinary close-calc placement: the placement freeze above is skipped
    // for recalc-born default market orders (strategy.entry MARKET and
    // strategy.order RAW alike), frozen_default_qty stays NaN, and the fill
    // kernels size with calc_qty(slipped fill) — open lots marked at the fill
    // (current_bar_ is the scheduler's point bar there). No KI-54 / gap-reject
    // / gross-admission snapshot is taken for them: those gates are pinned on
    // close-calc placements (their frozen invariant qty * sizing_price <=
    // sizing_equity has no meaning at a fill-time size); the zero-lot decline
    // and the affordability gate read the fill-time quantity. FIXED default
    // sizing and explicit quantities are untouched (never frozen); priced
    // (stop/limit) entries keep their own paths. Ordinary close executions
    // (coof_fill_recalc_active_ false) freeze exactly as before, so a script
    // without calc_on_order_fills is byte-identical.

    // KI-54 defect fix: the frozen sizing snapshot must see POST-liquidation
    // equity. TradingView liquidates intrabar, BEFORE the bar-close script
    // body runs; the engine's process_margin_call runs at the END of
    // dispatch_bar, AFTER on_bar placed (and froze) this bar's default-sized
    // market orders. When a margin call fires on the placement bar, the
    // frozen qty was computed on pre-liquidation equity — over-sized, so the
    // next bar's fill opens a position whose notional exceeds equity and the
    // long_full_margin branch of process_margin_call then emits a phantom
    // LONG margin call TV does not have. Rather than moving process_margin_call
    // (which would change what strategy.equity reads inside on_bar for every
    // strategy), the dispatch loop calls this refresh right after a margin
    // call actually liquidated something: every still-pending frozen
    // default-sized market order placed on THIS bar is re-frozen on the
    // post-liquidation state. Strict no-op on bars without a margin call
    // (the caller checks), and bit-identical recompute for untouched state.

    // --- Strategy variable accessors ---


    // KI-64: freeze the pre-close position for the script-visible position
    // accessor before an ordinary POOC strategy.close/close_all fills in-line
    // this bar. Capture-once per on_bar (a second same-bar close keeps the
    // FIRST pre-close snapshot). Caller guards close-timing mode &&
    // !immediately; this reads position_side_/position_qty_ while they still
    // hold the pre-close values (execute_immediate_close has not run yet).

    // KI-64: release the freeze so the next script-visible read returns the real
    // (post-close) position. Called at the top of flush_same_bar_close(), i.e.
    // immediately after every POOC on_bar returns.


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
        // Account-currency, matching emit_close_trade / open_trade_profit —
        // callers combine this with initial_capital_ + net_profit_sum_ (both
        // account-currency) to get total equity. fx=1.0 is a no-op.
        return diff * position_qty_ * syminfo_.pointvalue * active_account_currency_fx();
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

    // Chart-timezone-aware decomposition for the existing loss-day clocks
    // and the continuous/unconfigured-session order-counter fallback. The
    // order counter on an explicitly timed session instead consumes
    // source-layer intraday-cap risk-day policy, which follows the symbol's trading day.
    //
    // Falls back to plain ``_decompose_bar_time()`` (UTC) when no chart
    // timezone has been set, preserving the legacy fast path for
    // engine consumers that don't call ``set_chart_timezone``.
    //
    // Defined out-of-line in src/engine_risk.cpp so we can use the
    // private ``ScopedTimezone`` helper without leaking its header into
    // the public engine.hpp surface.
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
    bool barstate_islast_ = false;
    // Independent from barstate.isnew. False only when a COOF execution
    // restores a completed ordinary-close checkpoint that already contains
    // the current bar's one committed history slot.
    int magnifier_samples_ = 4;
    MagnifierDistribution magnifier_dist_ = MagnifierDistribution::ENDPOINTS;
    // When true, run_magnified_bar scales per-sub-bar sample count by
    // (sub_bar.volume / mean_sub_bar_volume) within each script bar — dense
    // tick approximation on high-volume sub-bars without real tick data.
    bool magnifier_volume_weighted_ = false;

    // KI-60 scheduler transients. Script executions see the complete
    // historical bar, while direct POOC/immediate market closes use the
    // monotonic broker cursor price held here.
    // finding-446: true when coof_cursor_price_ is a RAW OHLC path point /
    // magnifier tick (a broker-price fill there is nearest-tick rounded via
    // bar_fill_price); false when it is a resolved fill price (a bar-point
    // fill is already rounded, a level fill keeps its directional snap).
    // KI-67: true only while the active fill recalc owns the FIRST fill event
    // at the bar-open tick (O). Orders placed while this holds keep STANDARD
    // exact-level semantics. Later fills at that same O, like fills at every
    // other path point, are MID-BAR cascades (the Pine historical cascade permission).
    // True only while executing a fill recalc triggered by a later fill event
    // at O, after the first O fill has already consumed bar-open provenance.
    // Such a recalc is mid-bar for KI-67 and resumes on leg 0 (O->W1). This bit
    // lets strategy.exit apply the one pinned exception: a marketable LIMIT may
    // resume at W1, while marketable STOP suppression remains whole-entry-bar.
    // Round15: identify the MARKET opening whose first callback is active.
    // A direct close/partial/reentry in that body changes the serial and must
    // not inherit the original fill's permission to arm a recrossing limit.
    // KI-67: true only during a point-bar evaluation that sits AT an extreme
    // waypoint (W1 or W2) of the historical 4-tick path. Cascade orders born
    // this bar may fill only while this holds; on segments, at O, at C, and on
    // the ordinary-close / POOC-C / margin passes it is false so cascade orders
    // are held (they convert to ordinary resting orders at bar end). Set only by
    // the historical dispatch; the magnifier path never sets it.
    // KI-67 exit cascade: the historical dispatch publishes its current path
    // position here for the strategy.exit cascade gate. coof_hist_is_segment_
    // marks a segment (vs point) evaluation; coof_hist_path_index_ is the LEG
    // index (0..2) on a segment, or the path WAYPOINT index (0..3, cursor =
    // path[index]) on a point. Meaningful only while coof_scheduler_active_ on
    // the non-magnifier historical path; the POOC-C / margin passes publish the
    // C waypoint (index 3) so cascade exits are held there.
    // KI-67 exit cascade: the in-flight leg index (0..2) the CURRENT fill recalc
    // was triggered on — the leg the dispatch cursor traverses next after the
    // triggering fill. Published by the loop right before each recalc so a
    // strategy.exit placed in that recalc records its seg_i from the loop's real
    // position ("a fill AT a waypoint starts the NEXT leg"), rather than
    // re-deriving it from the fill price (ambiguous exactly at waypoints). -1 (or
    // >=3) outside a mid-bar historical recalc / at the terminal C tick.
    // KI-67 exit cascade: set by the gate immediately before evaluate_fill_price
    // so resolve_exit_path_fill runs its open-gap shortcut on the in-flight
    // leg-end waypoint POINT even when is_entry_bar (entry + exit share a bar).
    // Reset right after that evaluation; never set on the magnifier path.
    // Direct strategy.close / POOC fills can occur inside on_bar rather than
    // through process_next_pending_order. The scheduler refreshes this budget
    // before every speculative execution so those fills consume the same
    // finite historical/magnifier event budget as every other broker fill.
    // @broker-state begin
    // Monotonic cross-bar fill sequence counter; compared against
    // trail_best_before_bar_fill_seq_ (hashed above) and against
    // adapter placement fact `signal_close_mc_fill_seq` by fill-time gates
    // that cross the bar boundary.
    uint64_t broker_fill_event_seq_ = 0;
    // @broker-state end

    // --- Session predicate bar-state tracking ---
    // Tracks whether the previous bar was inside the regular session.
    // Used to compute session.isfirstbar (in_session && !prev_in_session_)
    // and session.islastbar (prev_in_session_ && !in_session).
    bool prev_in_session_ = false;
    // Current-bar session predicates — recomputed at the start of each bar
    // by set_session_bar_state() (engine_run.cpp) on every bar pump.
    bool session_ismarket_ = false;
    bool session_isfirstbar_ = false;
    bool session_islastbar_ = false;

    // session.ismarket of the CHART bar stamped bar_ms on the symbol's
    // session clock — the chart-timeframe-aware rule of session_time.hpp:
    // every bar of a daily-or-higher chart is the regular-session bar,
    // intraday bars keep the time-of-day test.
    bool chart_bar_ismarket(int64_t bar_ms) const;
    // Set the three per-bar predicates for the chart bar being dispatched.
    // in_session is chart_bar_ismarket(that bar); intraday_islastbar is the
    // pump's own lookahead verdict for an intraday chart (peek at the next
    // bar, barstate.islast, or never in magnifier mode). On a D/W/M chart
    // the bar IS the whole session — its own first and last bar — so
    // session.isfirstbar and session.islastbar both equal in_session there
    // and the prev/next bookkeeping does not apply.


    // --- Timeframe state ---
    std::string input_tf_;
    std::string script_tf_;
    // Cached tf_to_seconds(script_tf_). MUST be refreshed immediately after
    // every assignment to script_tf_ (both sites live in engine_run.cpp).
    // Avoids a string parse per strategy.* call.
    int script_tf_seconds_ = 0;
    TimeframeAggregator script_tf_agg_;
    int64_t prev_bar_timestamp_ = 0;

    // --- Historical -> realtime stream lifecycle ---
    // stream_begin() executes the historical warmup through the normal run()
    // path exactly once, then these fields carry the SAME broker, Pine series,
    // TA and timeframe-aggregator state forward while normalized trades arrive.
    enum class StreamPhase { IDLE, REALTIME, ENDED };
    StreamPhase stream_phase_ = StreamPhase::IDLE;
    bool stream_warmup_mode_ = false;
    // A host that keeps the legacy no-op refusal for in-run C-ABI setters
    // sets this for the handle lifetime so
    // guard_native_mutation stays a no-op (ab9714be LegacyCompatibilityConsumer::refuse).
    // Native hosts leave it false; their in-run setter still throws.
    bool host_mutation_guard_inert_ = false;
    int64_t stream_input_tf_ms_ = 0;
    int64_t stream_next_input_open_ms_ = 0;
    int64_t stream_clock_ms_ = 0;
    int64_t stream_last_tick_ms_ = 0;
    uint64_t stream_last_sequence_ = 0;
    bool stream_seen_sequence_ = false;
    bool stream_has_input_bar_ = false;
    Bar stream_input_bar_{};
    double stream_last_price_ = 0.0;
    bool stream_has_last_price_ = false;
    int stream_next_script_bar_index_ = 0;
    bool stream_script_bar_had_tick_ = false;
    bool stream_script_tick_seen_ = false;
    enum class StreamInputMode { UNSET, TICKS, BARS };
    StreamInputMode stream_input_mode_ = StreamInputMode::UNSET;
    bool stream_observe_actions_ = false;
    uint64_t stream_action_sequence_ = 0;
    std::vector<StreamOrderAction> stream_order_actions_;

    // --- request.security state ---
    struct HistoricalSecurityProjection {
        Bar bar{};
        // The instant the projection is dispatched on: the first retained
        // chart child's timestamp on the single-feed path, that child's first
        // auxiliary bar on the split-feed path (the requested-context
        // evaluator is fed the finer slice there). Keyed by instant, not by
        // feed-call index, so both paths consume one projection per bucket.
        int64_t first_child_ms = 0;
        bool is_complete = false;
    };

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
        // Requested-context bar index of the latest dispatch_security_eval()
        // (the ring address its TA members saw, see ta::bar_context()); -1
        // before the first dispatch. The calling-boundary replay re-dispatches
        // the same bar under the same index.
        int64_t ta_bar_index = -1;
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
        // Plain ``request.security`` (not ``_lower_tf``) with a requested TF
        // STRICTLY FINER than script_tf (e.g. so2TF="5" read from a 15m
        // chart) under ``lookahead=barmerge.lookahead_ON``: the security's
        // own aggregator completes multiple times
        // (script_seconds / requested_seconds) per calling/script bar. A
        // history-offset read (``expr[1]`` inside the security call, see
        // the ``*_hist`` push/read machinery in codegen) is meant to expose
        // "the value already confirmed as of the close of the PREVIOUS
        // calling bar" — TV's lookahead_on merge takes the FIRST intrabar
        // of each calling bar, so the publish granularity is the CALLING
        // bar, not the security's own (finer) period. Without this, the
        // read-before-push ``hist[0]`` gets refreshed on every one of the
        // R completions inside the current calling bar, so by the time
        // on_bar() reads it the value has silently drifted to "one
        // security-period behind the LAST completion of THIS SAME calling
        // bar" (e.g. the middle of 3 sub-periods) instead of "the last
        // completion of the PREVIOUS calling bar" — an aliasing bug
        // confirmed against TradingView-exported trades on a triple-RSI
        // DCA strategy using so2Rsi = request.security(sym, "5",
        // ta.rsi(close,7)[1], lookahead=barmerge.lookahead_on) on a 15m
        // chart (finer target under lookahead + offset).
        //
        // ``lookahead_OFF`` is deliberately NOT gated (field stays 0): TV's
        // lookahead_off merge takes the LAST intrabar of the calling bar,
        // so the exposed value — and any ``[k]`` history offset off it —
        // advances at the security's own finer cadence (one hist.push per
        // completed security period), which is exactly the ungated
        // behavior. Gating lookahead_off regressed
        // masayanfx-multi-time-score-strategy
        // (request.security(sym, "5", ta.highest(high, 20)[1],
        // barmerge.gaps_off, barmerge.lookahead_off) on a 15m chart) from
        // 100.0% to 93.7% trade parity vs TradingView.
        //
        // When nonzero, this holds the requested TF's duration in seconds
        // (script_seconds % this == 0 verified at validate time) and gates
        // ``feed_security_eval_state``'s aggregator branch: only the
        // completion whose bucket END aligns to a script_tf boundary is
        // passed through to ``evaluate_security`` as ``is_complete = true``
        // (letting codegen's ``hist.push()`` fire); all other completions
        // within the same calling bar are still evaluated (so the
        // underlying TA state keeps advancing at native/security
        // resolution) but are passed ``is_complete = false`` so they do not
        // advance the exposed history buffer. Zero (the default) means "not
        // applicable" (target TF coarser than or equal to script_tf, or
        // lookahead_off — the already-correct cases) and leaves behavior
        // unchanged.
        int publish_gate_tf_seconds = 0;
        // Plain ``request.security`` with a requested TF strictly finer than
        // script_tf, served by the auxiliary finer feed (the split-feed
        // path), under ``lookahead_off``: TradingView surfaces the LAST
        // intrabar of the calling chart bar at that bar's close whatever
        // the bucket's sub-bar count -- on the OANDA:XAUUSD 1D chart the
        // Thanksgiving 2025-11-26 bar's last 3m bucket (21:57Z, holding
        // the 21:59Z minute alone before the 22:00Z session close) is the
        // value ``request.security(tickerid, "3", ta.rsi(close, 14))``
        // reads at the daily close (72.64, lab tv dca-ltf-last-intrabar,
        // 2026-09-05), where the aggregator's count / real-end /
        // session-close rules leave that bucket partial until the next
        // chart bar's first sub-bar and the close read the 21:54Z bucket
        // (38.87). When set, feed_security_eval_state
        // finalizes and publishes the pending partial bucket on the
        // calling bar's last auxiliary bar (TimeframeAggregator::
        // complete_pending_partial), once: the next chart bar's first
        // sub-bar resets the bucket without re-emitting it. Dense feeds
        // whose final bucket completes on its count are untouched (no
        // partial is pending), and so are lanes without the auxiliary
        // slice, lookahead_on (its gated publication is untouched: on this
        // shape it stays one bucket behind, as before -- the tape pins
        // lookahead_off only), lower-TF arrays and calendar / same-TF
        // requests. False (the default) means "not applicable".
        bool calling_close_completes_partial = false;
        // Plain ``request.security`` with a requested TF strictly finer than
        // script_tf, served by the auxiliary finer feed (the split-feed
        // path), under ``lookahead_on``: TradingView's merge takes the FIRST
        // intrabar of the calling chart bar and holds it for the bar -- on
        // the BINANCE:BTCUSDT 1D chart ``request.security(tickerid, "15",
        // ta.rsi(close, 14)[1], lookahead_on)`` reads, on every daily bar,
        // the 15m RSI of the previous day's LAST bucket, i.e. ``rsi[1]``
        // evaluated on the day's first 15m bucket, na on the range's first
        // bar (lab tv notrade-ltf-sample-btc1d, 2025-04-01..20, 18/18,
        // 2026-09-05), so a plain ``expr`` reads the day's first bucket and
        // ``expr[k]`` the k-th bucket before it, at the requested cadence.
        // The legacy gate above (publish_gate_tf_seconds) publishes one
        // bucket per calling bar -- the LAST one -- which reads right for
        // ``expr[1]`` alone and one bucket late for ``expr``. When set, the
        // evaluator publishes EVERY completed requested bucket (the exposed
        // history advances per bucket, as under lookahead_off), and
        // feed_aux_security_for_chart_bar feeds the calling bar's auxiliary
        // bars only up to the one completing its FIRST bucket before the
        // chart body runs; the rest of the slice is held in ``deferred_aux``
        // and fed by feed_deferred_aux_security_for_chart_bar right after
        // dispatch_bar, so the body reads the first-bucket evaluation while
        // the TA state still sees every sub-bar, in order, before the next
        // chart bar. publish_gate_tf_seconds stays 0 on this path; lanes
        // without the auxiliary slice keep the gate. False (the default)
        // means "not applicable".
        bool calling_open_latches_first = false;
        // Per calling chart bar: whether this state's first bucket of the
        // slice has been published (the deferral point), and the auxiliary
        // bars held back until after the chart body, each with the
        // security_next_input_ms_ / calling_bar_complete it was fed with.
        bool first_bucket_published = false;
        // The label (bucket open) of the slice's first requested bucket:
        // a completion published by this slice's first auxiliary bars that
        // carries an OLDER label is the boundary emission of the previous
        // slice's still-pending bucket (a tail the count / real-end /
        // session-close rules left partial), not this bar's first bucket.
        int64_t slice_open_label = 0;
        // The label of the latest completed bucket this evaluator published
        // through its aggregator (feed_security_eval_state), whatever the
        // state's current bucket is afterwards.
        int64_t last_published_label = 0;
        struct DeferredAuxBar {
            Bar bar;
            int64_t next_input_ms = 0;
            bool calling_bar_complete = false;
        };
        std::vector<DeferredAuxBar> deferred_aux;
        // One entry per projected HTF bucket, populated only for an explicitly
        // opted-in finite historical batch. Empty for every default/streaming
        // run and for sites outside the narrow HTF lookahead_on+gaps_off
        // contract. The feed index advances once per retained input bar (bars
        // before an opt-in security range start are dropped by both producer
        // and consumer); the projection cursor advances only at the next
        // bucket's first child.
        std::vector<HistoricalSecurityProjection> historical_projections;
        std::size_t historical_projection_cursor = 0;
        // Which projection (cursor) has already been dispatched: every later
        // input of the same bucket is a no-op for the evaluator.
        bool historical_projection_dispatched = false;
        // Native higher-timeframe feed routing, rebuilt per run by
        // prepare_native_security_feeds(): the index into
        // native_security_feeds_ serving this state's requested timeframe
        // (-1: none, the aggregate stands), and that feed's bars keyed by
        // the label this state's aggregator stamps on the same bucket --
        // TimeframeAggregator::bar_label_ms of the native bar's covered
        // session instant -- so a completed bucket finds its exchange bar by
        // the timestamp the aggregate already carries. The same call installs
        // the feed's stamps into this state's aggregator as its period
        // partition (TimeframeAggregator::set_native_periods): the bucket's
        // span, label and completion bar are the native bar's own.
        int native_feed_index = -1;
        std::unordered_map<int64_t, Bar> native_bars_by_label;
    };

    std::vector<SecurityEvalState> security_eval_states_;
    // Boundary-fallback publication replays the completed caller's already
    // evaluated final requested value. Force generated TA sites down their
    // recompute path so the replay advances merged history only, never the
    // requested-context TA cadence. Keep this byte layout-unconditional so
    // generated strategy TUs and the statically linked runtime always agree on
    // BacktestEngine offsets.
    bool security_history_publication_replay_ = false;

    // The raw feed used by security aggregators in the active run. This is the
    // chart input TF on the legacy path and the auxiliary TF on the split path.
    std::string security_input_tf_;

    // Native higher-timeframe request.security feeds: the exchange's own
    // bars of one timeframe each (TradingView's "D" bar on an intraday chart
    // is the settlement / official close, not the last intraday close). A
    // completed request.security bucket of a fed timeframe takes the native
    // bar's OHLCV; timing, other timeframes, chart and broker are untouched.
    // Layout-unconditional like the aux feed's siblings above.
    struct NativeSecurityFeed {
        std::string tf;
        int seconds = 0;
        std::vector<Bar> bars;
    };
    std::vector<NativeSecurityFeed> native_security_feeds_;
    int64_t diag_native_security_substitutions_ = 0;
    int64_t diag_native_security_misses_ = 0;
    // The chart symbol's own daily partition, built per run from the native
    // "D" feed's stamps on an intraday chart (prepare_chart_day_partition)
    // and installed for the run's bar loop (NativeDayPartitionScope) so the
    // chart-level D consumers -- time("D") / time_close("D"),
    // timeframe.change("1D"), ta.change(time("D")), ta.vwap's daily anchor --
    // read TradingView's trade-date daily bars (timeframe.hpp). Empty on a
    // run without the feed or on a calendar chart: every rule nominal.
    NativeDayPartition chart_day_partition_;

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

    // Live-runtime surface (ABI v4). All default off/zero; historical runs are
    // byte-identical when untouched (tests/test_live_flags_off_identity.cpp).
    int last_run_status_ = 0;               // 0 completed, 1 NOT_COMPLETED (abort)

    // Cooperative abort (spec §3.5): set from any thread via request_abort();
    // consumed by the run in progress at the top of each bar-loop iteration
    // via check_abort(), which unwinds the run with AbortRequested. Cleared
    // at every run() entry, so a request made while idle is a no-op.
    //
    // AbortFlag wraps std::atomic<bool> in a copy/move-constructible shell:
    // std::atomic itself has its copy/move members deleted, and some tests
    // (e.g. tests/test_pooc_global_full_exit.cpp's ``run_case``) return a
    // BacktestEngine subclass by value, which needs the class to stay
    // implicitly copyable. Copying/moving never carries an in-flight abort
    // request across — there is no "run in progress" on a copy — so the
    // copy always starts cleared.
    struct AbortFlag {
        std::atomic<bool> value{false};
        AbortFlag() = default;
        AbortFlag(const AbortFlag&) noexcept {}
        AbortFlag(AbortFlag&&) noexcept {}
        AbortFlag& operator=(const AbortFlag&) noexcept {
            value.store(false, std::memory_order_relaxed);
            return *this;
        }
        AbortFlag& operator=(AbortFlag&&) noexcept {
            value.store(false, std::memory_order_relaxed);
            return *this;
        }
        bool load(std::memory_order order) const { return value.load(order); }
        void store(bool v, std::memory_order order) { value.store(v, order); }
    };
    AbortFlag abort_requested_;
    struct AbortRequested {};               // thrown inside the bar loops only
    void check_abort() {
        if (abort_requested_.load(std::memory_order_relaxed)) throw AbortRequested{};
    }

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
    // The one path to evaluate_security(): installs the requested context's
    // bar index for the evaluator's TA members (ta::bar_context()) for the
    // duration of the dispatch. `bar_index` is the 0-based index of the
    // requested-context bar being evaluated — the just-completed bucket for a
    // complete evaluation (eval_complete_count - 1), the in-progress bucket
    // for a partial/lookahead one (eval_complete_count) — so every
    // compute()/recompute() dispatch of one requested bar rewrites the same
    // ring slot, and a conditional window call inside the security expression
    // is addressed exactly like TradingView addresses it.
    void dispatch_security_eval(SecurityEvalState& state, const Bar& bar,
                                bool publish, int64_t bar_index);
    // KI-55 range-start gate for one evaluator: true when the input bar at
    // `input_ts` belongs to an HTF bucket that opened before the cut --
    // security_range_start_ms_ under the flag, else the run's first chart
    // bar for a coarser-than-chart / chart-timeframe evaluator
    // (security_first_chart_bar_ms_, split-feed runs with the auxiliary feed
    // proving prior trading, or single-feed historical intraday forex/cfd D
    // requests keyed to their session's actual open; false for lower TFs). The
    // progressive feed and the historical lookahead projection builder must
    // agree on this predicate so projected child indexes line up with the
    // per-state feed cursor.
    bool security_input_precedes_range_start(const SecurityEvalState& state,
                                             int64_t input_ts) const;
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    // True when the auxiliary request.security feed holds a bar in
    // [from_ms, to_ms): the evidence that an HTF bucket whose nominal open
    // precedes the run's first chart bar was in progress at the range start.
    bool aux_security_traded_between(int64_t from_ms, int64_t to_ms) const;
#endif
    void feed_security_eval_state(
        SecurityEvalState& state, const Bar& input_bar,
        bool calling_bar_complete = false);
    void publish_security_eval_state_at_calling_boundary(
        SecurityEvalState& state);

    // A new batch run (including stream_begin's historical warmup) starts a
    // fresh script lifecycle. Called once after broker reset, before any
    // requested/chart computation or per-bar checkpoint. Generated subclasses
    // reset their own persistent state here, then optionally prepare the
    // current run's static TA cache. Config/inputs/feeds belong to the engine
    // and survive. Streaming ticks and fill recalculations never call this.
    virtual void prepare_script_run(const Bar*, int, bool) {}

    virtual void configure_security_evaluators() {}
    virtual void evaluate_security(int sec_id, const Bar& bar, bool is_complete) {}
    virtual void clear_security(int sec_id) {}

    // Generated-state transaction hooks for calc_on_order_fills. Snapshot is
    // called once before the broker walks a historical bar; restore precedes
    // every fill recalc and the ordinary close execution. The completed
    // ordinary-close execution becomes the committed checkpoint. Historical
    // post-C fill recalculations start from it, recompute its current-bar
    // history slot, and are rolled back after their broker effects persist.
    virtual void snapshot_script_state() {}
    virtual void restore_script_state() {}
    virtual void commit_script_state() {}

    // Magnifier helpers


    virtual void finalize_bar() {}

    // --- Equity extremes update (called after each on_bar) ---
    // NOTE: the dd/runup walk in src/engine_metrics.cpp (compute_equity_stats)
    // MUST mirror this trough-reset logic; keep in lockstep. The fold is
    // exactly one per script bar and always paired with record_equity_point,
    // so the curve holds the very values folded here and a re-walk of the
    // curve through fold_equity_extreme reproduces the scalars bit for bit
    // — record_range_end_close_trades (engine_orders.cpp) relies on that
    // when it re-marks the last point.
    void fold_equity_extreme(double eq) {
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
    void update_equity_extremes() {
        fold_equity_extreme(initial_capital_ + net_profit_sum_ + open_profit(current_bar_.close));

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
    uint64_t closed_trade_entry_incarnation(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0;
        return trades_[idx].entry_incarnation;
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

    // Internal sizing helper; protected (alongside calc_qty) so the sizing-guard
    // test can exercise the fill_price<=0 / NaN rejection path directly. See
    // tests/test_adversarial_ohlcv.cpp.
protected:
    execution::Result settle_with_context_scoped(
        const execution::Action& action, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle,
        const execution::PhysicalExecutionContext& context,
        execution::CloseScope scope);
    execution::Result settle_with_context_selected(
        const execution::Action& action, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle,
        const execution::PhysicalExecutionContext& context,
        const execution::SelectedOpeningSet& selection);
    execution::AccountEffectProjection project_with_membership(
        const execution::Action& action,
        const execution::Fill& fill,
        execution::CloseScope book_or_opening,
        const execution::SelectedOpeningSet* selected) const;
    execution::SettlementInspection inspect_with_membership(
        const execution::Action& action,
        const execution::Fill& fill,
        execution::CloseScope book_or_opening,
        const execution::SelectedOpeningSet* selected) const;
    execution::Result settle_with_membership(
        const execution::Action& action, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle,
        const execution::PhysicalExecutionContext& context,
        execution::CloseScope book_or_opening,
        const execution::SelectedOpeningSet* selected);
    // Runtime-private preview of the exact existing pre-source prefix. The
    // caller pins the quoted ticket before entering this method.
    execution::Status preview_native_settlement_commit(
        const execution::Action& action, const execution::Fill& fill,
        const execution::PhysicalExecutionContext& context,
        execution::CloseScope scope, const execution::SelectedOpeningSet* selected,
        execution::AccountEffectProjection& account, std::vector<double>& row_pnl) const;
    execution::Status preview_native_settlement_commit(
        const execution::ReverseTo& reversal, const execution::Fill& fill,
        const execution::PhysicalExecutionContext& context,
        execution::AccountEffectProjection& account, std::vector<double>& row_pnl) const;
    struct NativeSettlementStage;
    struct NativeSettlementRows;
    execution::Status validate_native_settlement_book(double& held) const;
    execution::Status allocate_native_settlement_closes(
        NativeSettlementStage& stage,
        const execution::CloseScope& book_or_opening,
        double& remaining) const;
    void finish_native_settlement_stage(
        NativeSettlementStage& stage, const execution::Fill& fill) const;
    execution::SettlementInspection inspect_native_settlement_stage(
        const NativeSettlementStage& stage, const execution::Fill& fill) const;
    execution::AccountEffectProjection project_native_settlement_stage(
        const NativeSettlementStage& stage, const execution::Fill& fill) const;
    execution::Result commit_native_settlement_stage(
        NativeSettlementStage& stage, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle,
        const execution::PhysicalExecutionContext& context);
    void build_native_settlement_close_rows(
        const NativeSettlementStage& stage, const execution::Fill& fill,
        const execution::PhysicalExecutionContext& context,
        NativeSettlementRows& rows) const;
    execution::Status prepare_native_settlement_commit(
        const NativeSettlementStage& stage, const execution::Fill& fill,
        const execution::PhysicalExecutionContext& context,
        NativeSettlementRows& rows) const;
    execution::Status preflight_native_settlement_effects(
        const NativeSettlementStage& stage,
        const execution::LifecycleEffects& lifecycle,
        const NativeSettlementRows& rows);
    execution::Result commit_prepared_native_settlement_stage(
        NativeSettlementStage& stage, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle,
        const execution::PhysicalExecutionContext& context,
        NativeSettlementRows& rows);
    execution::Result settle_source_staged_execution(
        NativeSettlementStage& stage, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle,
        const execution::PhysicalExecutionContext& context);
    void stage_native_settlement(
        NativeSettlementStage& stage,
        const execution::Action& action,
        const execution::Fill& fill,
        execution::CloseScope book_or_opening,
        const execution::SelectedOpeningSet* selected,
        const execution::LifecycleEffects* lifecycle) const;
    void stage_native_settlement(
        NativeSettlementStage& stage,
        const execution::ReverseTo& reversal,
        const execution::Fill& fill,
        const execution::LifecycleEffects* lifecycle) const;
    enum class PositionReductionCause {
        SCRIPT_ORDER,   // strategy.close / close_all / market exit / reversal
        BRACKET_EXIT,   // a strategy.exit bracket leg fill
        MARGIN_CALL,
    };



    void append_same_side_fill(PyramidEntry lot);
    void append_quoted_lot(PyramidEntry lot, double total_qty, double average_price);
    // Allocates the new position cycle, lots and observations, then binds
    // exits that still remain in request_roster. Settlement that authorized
    // pending removals applies those erasures after old-cycle unbind and
    // before this opening bind.
    void open_quoted_position(PositionSide requested, PyramidEntry lot);

    void record_close_trade(Trade trade);
    void validate_close_trade_counters(const Trade* rows, size_t count) const;
    // Quote one resolved execution's current charges. Entry costs on the
    // closed rows are historical allocations. Returns close shares in FIFO
    // order followed by the opening share (zero when there is no opening).
    std::vector<double> quote_execution_commissions(
        const std::vector<double>& closed_units, double opening_units,
        const execution::Fill& fill) const;
    // The arithmetic of emit_close_trade without its bookkeeping: the Trade
    // row a close of ``close_qty`` of ``pe`` at ``fill_price`` on the
    // current bar would record (pnl, pnl_pct, commission, excursions, bar
    // indexes). emit_close_trade builds and commits; the range-end close
    // builds only.

    Trade build_close_trade_with_costs(const PyramidEntry& pe, double close_qty,
        double fill_price, bool was_long, double entry_commission,
        double exit_commission,
        const execution::PhysicalExecutionContext& context) const;
    void reset_position_state_to_flat();
    // Reset ALL per-run state (trades, accumulators, position, pending orders,
    // equity extremes, risk latches, intraday/day counters, source-series
    // history) so a reused handle's run N is bit-identical to a fresh handle's
    // run 1. Preserves configuration (initial_capital_, pyramiding_, slippage_,
    // commission_*, default_qty_*, syminfo_, inputs_, risk thresholds) — those
    // are set before run() and must survive it. Called at the top of every
    // run() loop entrypoint. See tests/test_handle_reuse_reset.cpp.
    void reset_run_state();
    double account_currency_fx_at(int64_t timestamp_ms) const;
    double active_account_currency_fx() const;
    void settle_position_after_partial_exit(
        PositionReductionCause cause);





    // `fill_price` is already resolved. Source sizing, direction and dust
    // selection stay here; purge_pending_exits is translated into exact
    // pending removals for the settlement coordinator. False does not
    // touch request_roster storage.








    // run() helpers (defined in engine_run.cpp).


    // Native HTF feed routing (engine_aux_security.cpp): per-state label maps
    // built after the evaluators' aggregators exist for this run, and the
    // substitution a completed bucket applies. Returns whether `bar` was
    // replaced by its native sibling.
    void prepare_native_security_feeds(const Bar* input_bars, int n_input);
    void prepare_chart_day_partition(const Bar* input_bars, int n_input);
    bool substitute_native_security_bar(SecurityEvalState& state, Bar& bar,
                                        bool count_miss = true);


#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    virtual bool source_aux_security_feed_enabled() const;
    virtual void source_aux_security_input_view(const Bar*& bars, int& n) const;
    bool aux_security_feed_enabled() const {
        return source_aux_security_feed_enabled();
    }

    // Neutral capability bridge for independent factorial patches. The
    // two-argument feed exists in the base engine. A completion-aware factor
    // may add a third bool argument; dependent-expression overload selection
    // forwards the native chart completion only when that capability exists.
    // Neither factor names or requires the other's feature macro.
    template <typename EngineT>
    static auto feed_security_at_calling_bar_boundary_impl(
            EngineT* engine, SecurityEvalState& state, const Bar& bar,
            bool calling_bar_complete, int)
        -> decltype(engine->feed_security_eval_state(
                        state, bar, calling_bar_complete), void()) {
        engine->feed_security_eval_state(state, bar, calling_bar_complete);
    }

    template <typename EngineT>
    static void feed_security_at_calling_bar_boundary_impl(
            EngineT* engine, SecurityEvalState& state, const Bar& bar,
            bool, long) {
        engine->feed_security_eval_state(state, bar);
    }

    void feed_security_at_calling_bar_boundary(
            SecurityEvalState& state, const Bar& bar,
            bool calling_bar_complete) {
        feed_security_at_calling_bar_boundary_impl(
            this, state, bar, calling_bar_complete, 0);
    }

#endif
    // Runs the standard per-script-bar order/strategy sequence on current_bar_:
    //   request matching -> update_per_trade_extremes -> on_bar,
    // plus a second request matching when close-timing mode is set
    // (TV process_orders_on_close: new market orders fill at this bar's close).
    // Shared by run(), run_simple_bar_loop, and the no-magnifier aggregation
    // path. The magnifier tick loop does NOT use this — it gates the sequence
    // on is_last_tick_ and forces is_first_tick_ before on_bar.




    // Live-runtime tail (spec §3.1): once script_tf_seconds_ is known for
    // this run, freeze pine_last_bar_index()/last_bar_time_ at the horizon
    // bar instead of the fed array's actual last index. No-op unless
    // realtime_tail_ is on and realtime_tail_horizon_bars_ > 0.
    //
    // script_bar_geometry selects which timestamp rule applies to
    // last_bar_time_ (last_bar_index_ = horizon - 1 either way):
    //   true  -- `bars` IS the script-bar array (the single-TF run(bars, n)
    //            path, and run_tf_impl's !needs_aggregation call, where
    //            input_tf == script_tf so input bars ARE script bars):
    //            exact bars[horizon - 1].timestamp when horizon <= n, else
    //            extrapolated from bars[n - 1] one script-TF step per
    //            missing bar past the array's last bar.
    //   false -- `bars` is the *input* array under aggregation
    //            (needs_aggregation, input_tf < script_tf): indexing it by
    //            a script-bar horizon would land on the wrong input bar
    //            (final-rereview.md N1), so instead extrapolate from the
    //            first input bar's timestamp, one script-TF step per
    //            horizon bar (the pre-fix formula, restored for this path
    //            only).
    void apply_realtime_tail_horizon(const Bar* bars, int n,
                                      bool script_bar_geometry);
    // The TF-aware run()'s actual work (dispatch loop selection, the
    // try/catch, both cleanup paths). Does NOT touch last_error_,
    // last_run_status_, or abort_requested_ -- every public run() overload
    // clears those exactly once at its own entry before reaching here, so a
    // request_abort() arriving during a delegating overload's own setup
    // (e.g. the SymInfo/overrides overload's syminfo/inputs copy) is never
    // silently wiped by a second, later clear.
    // The caller has already validated the complete chart bar array; do not
    // rescan here or clear an abort that arrived during preflight/setup.

    void stream_observe_entry(const PyramidEntry& pe);
    virtual void source_stream_entry_comment(const PyramidEntry&, std::string&) const;
    void stream_observe_exit(size_t trade_index);
    void stream_refresh_action_metadata(size_t first_action, size_t first_trade);

    // fill_report helpers (defined in engine_report.cpp).
    void fill_trades_section(ReportC* out) const;
    void fill_metrics_section(ReportC* out) const;
    void fill_security_diag_section(ReportC* out) const;
    void fill_trace_section(ReportC* out) const;

    void guard_native_mutation(const char* operation);

    struct ExecutionConsumerSlot {
        bool native = false;
        mutable std::unique_ptr<IExecutionConsumer> ptr;
        ExecutionConsumerSlot() = default;
        ExecutionConsumerSlot(const ExecutionConsumerSlot& other) noexcept
            : native(other.native) {}
        ExecutionConsumerSlot& operator=(const ExecutionConsumerSlot& other) noexcept {
            if (this != &other) {
                native = other.native;
                ptr.reset();
            }
            return *this;
        }
        ExecutionConsumerSlot(ExecutionConsumerSlot&& other) noexcept : native(other.native) {
            other.ptr.reset();
        }
        ExecutionConsumerSlot& operator=(ExecutionConsumerSlot&& other) noexcept {
            if (this != &other) {
                native = other.native;
                ptr.reset();
                other.ptr.reset();
            }
            return *this;
        }
    };
    ExecutionConsumerSlot execution_consumer_slot_;

public:
    virtual ~BacktestEngine();
    int execution_contract() const;
    bool native_bound() const;
    virtual void on_bar(const Bar& bar) = 0;

    // All run() overloads preflight the entire chart array before modifying
    // configuration, resetting state, preparing scripts or dispatching bars.
    // Require n >= 0, a non-null pointer when n > 0, finite enclosed OHLC,
    // volume that is finite >= 0 or NaN (unavailable), and strictly increasing
    // timestamps with representable positive int64 deltas. Finite signed/zero
    // prices, off-grid prices and gaps are structurally admitted, without a
    // guarantee about their financial or extreme-calendar arithmetic.
    // Rejection sets last_error() and preserves state except entry-time error,
    // run-status and abort bookkeeping. n == 0 retains empty-new-run semantics.
    // Processing exceptions after preflight do not imply state rollback.
    void run(const Bar* bars, int n);

    void run(const Bar* input_bars, int n_input,
             const std::string& input_tf,
             const std::string& script_tf,
             bool bar_magnifier = false,
             int magnifier_samples = 4,
             MagnifierDistribution magnifier_dist = MagnifierDistribution::ENDPOINTS);

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    // Copies a finer request.security-only feed for subsequent historical
    // runs. n == 0 clears it. Validation that depends on native chart bars is
    // intentionally deferred to run(), where failures reach last_error().
    virtual bool set_aux_security_feed(const Bar* bars, int n,
                               const std::string& input_tf);
#endif

    // Copies the exchange's own bars of one higher timeframe for the
    // request.security evaluators that request exactly it (see
    // strategy_set_native_security_feed in pineforge.h). n == 0 clears that
    // timeframe's feed. Routing is built per run once the evaluators exist.
    bool set_native_security_feed(const std::string& timeframe,
                                  const Bar* bars, int n);
    bool native_security_feed_enabled() const {
        return !native_security_feeds_.empty();
    }
    // Per-run diagnostics: completed buckets that took a native bar, and
    // completed buckets of a fed timeframe that found none (kept aggregate).
    int64_t native_security_substitutions() const {
        return diag_native_security_substitutions_;
    }
    int64_t native_security_misses() const {
        return diag_native_security_misses_;
    }
    // Whether the last run built the chart symbol's native daily partition
    // (an intraday chart with a native "D" feed installed).
    bool chart_day_partition_installed() const {
        return !chart_day_partition_.empty();
    }

    // Execute confirmed historical bars, then keep this exact instance alive
    // for realtime trade updates. The warmup feed must contain at least one
    // complete input-timeframe bar. Normalized ticks begin at or after the next
    // input bar's open; in-session gaps are materialized as zero-volume
    // carry-forward bars when a later tick or stream_advance_time() crosses
    // their close boundary. Configured out-of-session intervals are skipped.
    bool stream_begin(const Bar* warmup_bars, int n_warmup,
                      const std::string& input_tf,
                      const std::string& script_tf = "");
    // Confirmed input bars use the same OHLC broker kernel as batch. The first
    // tick/bar locks the input mode; mixing them is rejected. Missing in-session
    // bars are rejected rather than padded. Closed-session gaps may be skipped.
    bool stream_push_bar(const Bar& bar);
    bool stream_push_tick(const TradeTick& tick);
    bool stream_push_ticks(const TradeTick* ticks, int n);
    bool stream_advance_time(int64_t timestamp_ms);
    bool stream_end(bool finalize_partial_input_bar = false);
    bool stream_is_realtime() const { return stream_phase_ == StreamPhase::REALTIME; }
    int stream_order_actions_len() const { return static_cast<int>(stream_order_actions_.size()); }
    const StreamOrderAction& stream_order_action_at(int i) const {
        return stream_order_actions_.at(static_cast<size_t>(i));
    }
    void stream_order_actions_clear() { stream_order_actions_.clear(); }
    // Observable broker + stream cursors/forming bars, not a serialization of
    // arbitrary strategy members. Deterministic strategy code is required.
    uint64_t stream_state_hash() const;

    // Install an effective-time FX curve (account-currency units per one unit
    // of symbol quote currency). Points are copied and must have strictly
    // increasing epoch-ms timestamps plus positive finite rates. The latest
    // point whose timestamp is <= the current broker event is active. Passing
    // n=0 clears the curve and restores the scalar metadata fallback.
    bool set_account_currency_fx_series(const int64_t* timestamps_ms,
                                        const double* rates, int n);

    void run(const Bar* input_bars, int n_input,
             const std::string& input_tf,
             const std::string& script_tf,
             const std::unordered_map<std::string, std::string>& inputs,
             const SymInfo& syminfo,
             // Opaque host-override handle: the kernel only forwards it to
             // prepare_native_begin() as NativeBeginArgs::overrides_opaque
             // and never dereferences it. A host layer that defines its own
             // override record passes its address and casts it back there.
             const void* overrides = nullptr,
             bool bar_magnifier = false,
             int magnifier_samples = 4,
             MagnifierDistribution magnifier_dist = MagnifierDistribution::ENDPOINTS);

    int trade_count() const { return (int)trades_.size(); }
    const Trade& get_trade(int i) const { return trades_[i]; }
    // The same closed rows under the `closed_trade_*` name a host reads them
    // by (RP6). The per-field `closed_trade_*` family below stays protected
    // Pine plumbing: one whole row by reference answers all of it, and a host
    // that owns its report needs the row, not twenty wrappers. Range-end
    // report rows are NOT here — report_trade_count() / get_report_trade()
    // below span both spaces. Unchecked, like get_trade above: index against
    // closed_trade_count().
    std::size_t closed_trade_count() const noexcept { return trades_.size(); }
    const Trade& closed_trade(std::size_t i) const { return trades_[i]; }
    // The REPORT's row space: trades_ followed by range_end_trades_, in the
    // order fill_trades_section lays pf_report_t::trades out. trade_count()
    // / get_trade() stay the Pine-visible closed trades (strategy.closedtrades
    // never sees a range-end row); the report-indexed C ABI accessors
    // (strategy_closed_trade_entry_incarnation) must index THIS space, or
    // every range-end row reads as index-out-of-range (round-4b F3).
    int report_trade_count() const {
        return (int)(trades_.size() + range_end_trades_.size());
    }
    const Trade& get_report_trade(int i) const {
        const int n_closed = (int)trades_.size();
        return i < n_closed ? trades_[(size_t)i]
                            : range_end_trades_[(size_t)(i - n_closed)];
    }

    // ABI v4 live-runtime surface (task 9): classify why a REPORT-row
    // closed trade exited -- report-row scope (spans trades_ then
    // range_end_trades_, like get_report_trade above), so this cannot reuse
    // the existing protected closed_trade_* names below (strategy.
    // closedtrades.* scope: trades_ only, std::string returns). 0 UNKNOWN
    // (bad index), 1 SCRIPT, 2 BRACKET, 3 MARGIN_CALL, 4 INTRADAY_LOSS_CAP,
    // 5 INTRADAY_FILL_CAP, 6 RANGE_END. Defined in engine_trade_accessors.cpp;
    // see strategy_closed_trade_close_cause (pineforge.h) for the exact
    // derivation order.
    int closed_trade_close_cause(int i) const;

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
        guard_native_mutation("set_input");
        inputs_[key] = value;
    }
    void clear_inputs() {
        guard_native_mutation("clear_inputs");
        inputs_.clear();
    }
    void set_trade_start_time(int64_t timestamp_ms) {
        guard_native_mutation("set_trade_start_time");
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
        guard_native_mutation("set_chart_timezone");
        chart_timezone_ = tz;
    }
    const std::string& chart_timezone() const { return chart_timezone_; }

    // --- Symbol metadata injection (data feed → syminfo.*) ---
    // The exchange timezone + session feed session.ismarket / time(session)
    // predicates. They default to UTC / 24x7 (crypto); a data feed pushes
    // the real values via these setters before run().
    //
    // The source-layer intraday cap consumes a valid explicit session and this
    // timezone through its risk-day policy. Other risk-day rules keep
    // chart_timezone_, as do continuous/unconfigured order-counter clocks;
    // the existing crypto-on-shifted-chart contract therefore remains intact.
    void set_syminfo_timezone(const std::string& tz) {
        guard_native_mutation("set_syminfo_timezone");
        syminfo_.timezone = tz;
    }
    void set_syminfo_session(const std::string& s) {
        guard_native_mutation("set_syminfo_session");
        syminfo_.session = s;
    }
    // ``syminfo.type`` ("crypto" default; "forex" / "stock" / "futures" /
    // "index" / "fund" / "cfd" per TradingView). Scripts branch on it for
    // instrument conventions — the canonical one being the pip size
    // (``syminfo.type == "forex" ? 0.0001 : syminfo.mintick``), which on a
    // 5-digit FX symbol under the crypto default computed every pip-scaled
    // stop/target 10x too tight (finding 454). Empty is ignored.
    void set_syminfo_type(const std::string& t) {
        guard_native_mutation("set_syminfo_type");
        if (!t.empty()) syminfo_.type = t;
    }
    /// Whether TradingView's session template for this symbol carries the
    /// exchange's early closes and holidays, so a D/W/M request.security
    /// bucket completes on a shortened session's actual last chart bar
    /// (TimeframeAggregator::set_early_close_completes): exchange-listed
    /// kinds (stock, futures, index, fund, dr, ...) yes -- NYSE:F's 12:45 ET
    /// half-day bar and CME's 11:45 CT early-close bar are pinned; the OTC
    /// quote streams -- forex, cfd, crypto -- no: their period ends at the
    /// nominal close and a session ending early completes lazily on the
    /// next period's first bar (OANDA:XAUUSD 15m, lab tv oanda pin, ledger
    /// log-20260905t034240z-30be11fe). syminfo.type comes from the harness
    /// (strategy_set_syminfo_type <- PINEFORGE_VERIFY_SYMTYPE); the
    /// constructor default "crypto" keeps an untyped run on the lazy rule.
    bool session_template_knows_early_close() const;
    // Generic string-field injection for the remaining OHLCV-less syminfo
    // members (ticker / tickerid / currency / basecurrency / description /
    // volumetype / type). Unknown keys and empty values are ignored; returns
    // true when a field was set.
    bool set_syminfo_string(const std::string& key, const std::string& value) {
        guard_native_mutation("set_syminfo_string");
        if (value.empty()) return false;
        if (key == "type") { syminfo_.type = value; return true; }
        if (key == "ticker") { syminfo_.ticker = value; return true; }
        if (key == "tickerid") { syminfo_.tickerid = value; return true; }
        if (key == "currency") { syminfo_.currency = value; return true; }
        if (key == "basecurrency") { syminfo_.basecurrency = value; return true; }
        if (key == "description") { syminfo_.description = value; return true; }
        if (key == "volumetype") { syminfo_.volumetype = value; return true; }
        return false;
    }
    // Runtime syminfo injection (by design — the engine stores no instrument
    // metadata of its own; the harness supplies it per run). mintick drives the
    // directional fill snap + slippage*tick economics; pointvalue is the
    // futures $-per-point multiplier applied to every money path (realized
    // PnL + excursions, open profit / equity, percent/cash sizing, percent
    // commission notionals, margin check — see tests/test_pointvalue.cpp).
    // Both default to crypto/equity values (0.01 / 1.0) and only matter when the
    // harness sets a non-default instrument.
    void set_syminfo_mintick(double m) {
        guard_native_mutation("set_syminfo_mintick");
        if (m > 0.0) { syminfo_.mintick = m; syminfo_mintick_ = m; }
    }
    void set_syminfo_pointvalue(double pv) {
        guard_native_mutation("set_syminfo_pointvalue");
        if (pv > 0.0) { syminfo_.pointvalue = pv; }
    }

    // Toggle TradingView's forced-liquidation (margin call) emulation. Defaults
    // ON to match TV; set false for the legacy hold-the-position behaviour.
    void set_margin_call_enabled(bool enabled) {
        guard_native_mutation("set_margin_call_enabled");
        margin_call_enabled_ = enabled;
    }
    bool margin_call_enabled() const { return margin_call_enabled_; }
    virtual void set_syminfo_metadata(const std::string& key, double value);

    // Returns the script's active timeframe string (e.g. "15" for 15-minute,
    // "D" for daily). Backs timeframe.main_period in generated Pine v6 code.
    const std::string& main_period() const { return script_tf_; }

    // Live-runtime tail semantics (spec §3.1, ABI v4): the caller's fed array
    // ends with a still-forming bar rather than the chart's rightmost
    // historical bar. When `on`, the LAST bar of every subsequent run() (this
    // is persistent configuration, not a one-shot flag -- it stays set until
    // a caller passes on=false, and reset_run_state() does not touch it)
    // gets barstate.islast == false, session.islastbar computed from the
    // bucket calendar (no i+1 bar to peek at), pine_last_bar_index() /
    // last_bar_time_ frozen at the horizon bar (`horizon_bars - 1`), and no
    // range-end close row/trade. Default off: every historical run is
    // byte-identical to before this flag existed.
    void set_realtime_tail(bool on, int horizon_bars) {
        guard_native_mutation("set_realtime_tail");
        realtime_tail_ = on;
        realtime_tail_horizon_bars_ = horizon_bars;
    }
    bool realtime_tail() const { return realtime_tail_; }

    // Live probe tail suppression (spec §3.2, ABI v4): when `on`, the LAST
    // bar of every subsequent run() runs only dispatch_bar()'s pre-on_bar
    // broker steps (intraday-cap deferred close, _push_source_series,
    // request matching, evaluate_max_intraday_loss_over_path,
    // update_per_trade_extremes) and returns — on_bar is never invoked for
    // that bar, and nothing after it runs (no flush_same_bar_close, no POOC
    // second pass, no process_margin_call, no settle_dormant_bracket_
    // reissues, no sizing refresh). Margin-call / intraday-cap closes
    // therefore surface only at settlement (the next non-suppressed run),
    // not against the still-forming probe bar. This is persistent
    // configuration, like set_realtime_tail, and independent of it — do not
    // couple the two flags.
    // Honoured only on the standard dispatch_bar path (single-TF run loop,
    // run_simple_bar_loop). Silent no-op under calc_on_order_fills (COOF
    // scheduler) and under the bar magnifier (run_magnified_bar) -- both
    // gated in live v1. Semantics UNDEFINED on the non-magnifier aggregation
    // path (input_tf < script_tf) until the partial-bucket forming-bar flag
    // lands; see pineforge.h.
    // Default off (@p on == 0): every historical run stays byte-identical to
    // before this flag existed.
    void set_probe_suppress_tail_logic(bool on) {
        guard_native_mutation("set_probe_suppress_tail_logic");
        probe_suppress_tail_logic_ = on;
    }
    bool probe_suppress_tail_logic() const { return probe_suppress_tail_logic_; }

    // Force this run's intrabar path order (ABI v4 live-runtime surface,
    // task 4): 0 AUTO (the unchanged |H-O| vs |O-L| rule), 1 HIGH_FIRST
    // (O -> H -> L -> C), 2 LOW_FIRST (O -> L -> H -> C). Any other value is
    // clamped to AUTO. A live probe runs the SAME forming bar under both
    // forced orders and keeps only the fills that agree between the two --
    // a fill that depends on which leg TradingView's own still-forming bar
    // will resolve to is path-dependent and must be suppressed rather than
    // guessed. See internal::bar_path_uses_high_first's thread-local
    // override (engine_path_resolve.cpp) and the PathOrderScope guard in
    // engine_run.cpp that installs/clears it for exactly the duration of
    // this run's own dispatch.
    // Persistent configuration, like set_realtime_tail -- stays set until a
    // caller passes mode=0. The legacy route installs it through
    // PathOrderScope; a native-bound source provider projects the same value
    // into NativeRunSpec::path_order.
    // Default AUTO (mode=0): every historical run stays byte-identical to
    // before this flag existed.
    void set_path_order(int mode) {
        guard_native_mutation("set_path_order");
        path_order_mode_ = (mode == 1 || mode == 2) ? mode : 0;
    }

    // The dual-entry-stop arbitration decided on the LAST bar this run
    // dispatched (a flat position resting one long stop-only ENTRY and one
    // short stop-only ENTRY, both touched that bar --
    // dual_entry_stop_path_winner, engine_path_resolve.cpp): 0 None (no
    // such pair was arbitrated on that bar), 1 LongFirst, 2 ShortFirst --
    // internal::DualEntryStopPathWinner's own enumerator order (its Tie
    // value never reaches here; dual_entry_stop_path_winner always resolves
    // a tie to LongFirst). This reads last_bar_dual_entry_decision_, a
    // per-bar snapshot of the arbitration that survives whatever the
    // working state (dual_entry_path_) does afterward this same bar -- a
    // fill, a declined stop-entry admission, or (under
    // process_orders_on_close) the bar's second request matching
    // pass, all of which reset dual_entry_path_ to None without undoing the
    // fact that an arbitration happened. Only the standard
    // (non-calc_on_order_fills) dispatch path updates it; this is a silent
    // no-op (stays at its last standard-path value) under the COOF
    // scheduler, mirroring set_probe_suppress_tail_logic's
    // dispatch-path-scope caveat.

    // Live runtime G1 (spec §3.4): a deterministic, order-independent
    // FNV-1a 64 hash over every piece of broker state that decides the next
    // bar's fills (position, book, pyramid lots, trail scalars, cycle/
    // intraday/risk latches, frozen sizing, equity sums). Two engines with
    // equal broker state hash equally regardless of unordered-container
    // insertion history; any difference in that state changes the hash.
    // Implemented in engine_state_hash.cpp; coverage of the marked
    // broker-state region(s) below (grep this file for "broker-state") is
    // enforced by scripts/check_broker_state_hash_coverage.py.
    uint64_t broker_state_hash() const;

    // ABI v4 live-runtime surface (task 7, spec 3.6): read-only view of the
    // resting-order book after the most recent run() -- the book in force
    // for the next bar, in the vector's own (insertion) order; fill
    // priority is decided at fill time from created_seq. The C ABI
    // (strategy_pending_orders_len / strategy_pending_order_get) copies
    // each live request out through PendingIntentView's POD projection
    // (pf_pending_order_v1_t, include/pineforge/pending_order_mirror.hpp),
    // never by pointer. `i` must be in [0, pending_order_count()).

    // ABI v4 live-runtime surface (task 8, spec 3.6): engine-computed
    // derived values of a resting order and the position scalars the live
    // runtime would otherwise have to re-derive. Pure const reads of the
    // engine's own sizing / admission / level-resolution predicates; none
    // of them mutates the engine, so a historical run is byte-identical
    // whether or not a caller reads them. The source adapter derives the
    // projection from native requests, live state, and placement facts.
    //
    // probe_fill_qty: the quantity the entry kernel would open if the
    // order at `index` filled at `fill_price`, and which sizing partition
    // produced it -- exactly the "quantity the market / priced-entry kernel
    // would actually open with" computation of the zero-lot decline gate in
    // apply_filled_order_to_state, tagged:
    //   0 EXPLICIT               a script-supplied qty: for strategy.entry
    //                            calc_qty_for_type at the slipped fill
    //                            (apply_qty_step of the contracts for FIXED,
    //                            the budget sized at the fill for a per-call
    //                            percent/cash override); a strategy.order
    //                            explicit qty is dispatched VERBATIM
    //                            (apply_raw_order_fill, no lot step).
    //   1 FROZEN_PLACEMENT       a quantity the engine fixed before the fill,
    //                            never re-derived from the fill price:
    //                            frozen_default_qty (the default
    //                            percent_of_equity / cash MARKET or
    //                            strategy.order size at the signal close), a
    //                            MARKET's frozen broker transaction
    //                            (paired_flat_market_transaction_qty; a
    //                            same-bar-market transaction's frozen total from
    //                            FLAT or as a kept over-cap add), or what one
    //                            of the two MARKET reversal kernels opens:
    //                            a same-bar-market member against an
    //                            opposite live position opens the remainder
    //                            transaction_units - min(transaction_units, live qty)
    //                            (apply_same_bar_market_tx_reversal), and the
    //                            exact SHORT-seed collision's final short
    //                            re-opens the residual pyramid_entries_[0].qty
    //                            - pyramid_entries_[1].qty after closing both
    //                            lots (short_seed_collision_final_short_is_
    //                            live). Both kernels are modelled; each is
    //                            reported with close_only = 1 when it opens
    //                            nothing.
    //   2 DEFAULT_STOP_PLACEMENT default_stop_placement_qty, when
    //                            use_default_stop_placement_qty says
    //                            dispatch consumes it (round 7 family K).
    //   3 AT_FILL                default sizing at the slipped fill,
    //                            calc_qty(fill).
    // `fill_price` is slipped the way the kernel slips it (apply_slippage; a
    // LIMIT-triggered entry fills limit-or-better and is not slipped, see
    // docs/pine-adapter-kernel-notes.md).
    // `close_only` is 1 when the kernel's close-only predicate fires -- the
    // fill closes against the live opposite position and that predicate
    // opens no leg of its own (where the order was created FLAT the branch
    // is close_opposite_then_enter: a transaction larger than the live
    // position still opens the remainder, so a consumer compares `qty` with
    // the live position): the order's
    // affordability_close_only (entry leg declined at placement), the
    // priced-entry prior_cycle_close_only rule (opposite live position,
    // created_position_side != position_side_, and not a KI-65
    // reverses_same_bar_market_from_flat), the same-cycle frozen
    // explicit-FIXED transaction that the close consumes exactly, or a
    // finalized flat MARKET pair, or one of the two reversal kernels above
    // opening nothing -- each spelled as apply_entry_order_fill /
    // apply_market_order_fill spell it. A replaced default-percent short
    // (replaced_percent_short_market_is_live) is dispatched
    // close_opposite_then_enter with its frozen_default_qty: `qty` is that
    // transaction, close_only 0. Not folded into `qty`: the
    // deferred-flip carry (tv_carry_qty, enter_market_from_flat's
    // tv_deferred_flip rule adds it on top of this quantity for a priced
    // entry firing from FLAT whose placement side is the opposite of the
    // requested side) -- the mirror exposes tv_carry_qty and
    // created_position_side verbatim. Returns 0 on success; 1 (qty NaN,
    // close_only 0, partition -1) when the order is an EXIT, whose fill
    // quantity is decided against the live position at the fill, not by a
    // sizing partition; -1 on a bad index or a null out-pointer.

    // 1 when the order's entry-relative offsets (profit_ticks / loss_ticks /
    // trail_points) resolve NOW: entries, plain orders and exits with an
    // empty from_entry always; an exit bound to from_entry only once that
    // id has filled in the CURRENT position cycle (cycle_filled_entry_ids_,
    // the gate materialize_relative_exit_prices_for_live_position and the
    // eligibility pass share). 0 otherwise, -1 on a bad index.

    // The price levels the order would fire at, as the fill path resolves
    // them: a set stop_price / limit_price / trail_price verbatim (they are
    // already on the price grid); an unset leg from its tick offset against
    // position_entry_price_ when pending_order_level_resolved() == 1 and a
    // position is live, with the position side's sign exactly as
    // materialize_relative_exit_prices_for_live_position (limit = entry +
    // dir * profit_ticks * mintick, stop = entry - dir * loss_ticks *
    // mintick, dir = +1 long / -1 short, level_on_price_grid) and
    // resolve_exit_path_fill (activation = snap_trail_level_to_tick_grid(
    // entry +/- ceil(trail_points - 5e-5) * mintick); trail_points wins
    // over trail_price when both are set). NaN for a leg that is unset or
    // unresolvable. Returns 0, or -1 on a bad index / null out-pointer.

    // The live position's volume-weighted average entry price
    // (position_entry_price_; 0 when flat -- the engine keeps 0 there, the
    // C ABI reports NaN when flat), its cycle id (position_cycle_seq_; 0
    // when flat, a fresh nonzero id per open/reversal, kept across
    // same-direction adds) and the trail extreme the exit trail legs ride
    // (trail_best_price_; NaN until a position fills).
    double position_avg_price() const { return position_entry_price_; }
    int64_t position_cycle_seq() const { return position_cycle_seq_; }
    double trail_best_price() const { return trail_best_price_; }
    // ABI v4 live-runtime surface (task 9): public forwarders for the C
    // ABI, which -- being extern "C" free functions -- cannot reach the
    // protected signed_position_size() / current_equity() above.
    // signed_position_size() is strategy.position_size (KI-64 freeze-aware:
    // reads the pre-close position while a same-bar POOC close is frozen).
    // current_equity() is initial capital plus realized net profit
    // (strategy.initial_capital + strategy.netprofit). NOT Pine's
    // strategy.equity, which adds open profit on top of this (see the
    // sizing_equity formula and the equity-curve remark below, both
    // current_equity() + open_profit(...)).
    virtual double live_position_size() const {
        if (position_side_ == PositionSide::LONG) return position_qty_;
        if (position_side_ == PositionSide::SHORT) return -position_qty_;
        return 0.0;
    }
    virtual int observe_last_bar_dual_entry_path_v1() const;
    virtual int observe_pending_count_v1() const;
    virtual int observe_pending_copy_v1(int index, pf_pending_order_v1_t* out) const;
    virtual int observe_probe_fill_qty(int index, double fill_price, double* qty,
                                       int* close_only, int* partition) const;
    virtual int observe_pending_level_resolved(int index) const;
    virtual int observe_pending_effective_levels(int index, double* stop,
                                                 double* limit,
                                                 double* trail_activation) const;
    virtual double observe_trail_best_price_v1() const;
    double live_current_equity() const { return current_equity(); }
    // ABI v4 live-runtime surface (task 9): total SCRIPT bars dispatched by
    // the most recent run() (mirrors pf_report_t::script_bars_processed,
    // engine_report.cpp), including the stream warmup leg and every
    // realtime tick-driven bar after strategy_stream_begin.
    int64_t script_bars_processed() const { return diag_script_bars_processed_; }

    // ABI v4 live-runtime surface (task 6): when on, every script bar's
    // dispatch (all four script-bar dispatch sites -- the single-TF run()
    // loop, run_simple_bar_loop, run_aggregation_bar_loop, and
    // stream_dispatch_script_bar, engine_stream.cpp, the realtime-stream
    // continuation of a stream_begin warmup) appends broker_state_hash()
    // to broker_state_hashes_ immediately after that bar's
    // record_equity_point() call, so the recorded array's length matches
    // script_bars_processed and pf_report_t::broker_state_hash_len 1:1 --
    // including on strategy_stream_fill_report, whose report is the
    // cumulative warmup + realtime run. Default off: broker_state_hashes_
    // stays empty, fill_report emits a null/zero-length array, and every
    // historical run stays byte-identical to before this flag existed.
    // reset_run_state() clears the recorded array on every run() (the
    // warmup leg of stream_begin included) regardless of this flag's
    // value; the flag itself is persistent configuration, like
    // set_realtime_tail, so it must be set BEFORE stream_begin to also
    // cover the warmup bars.
    void set_broker_state_hash_recording(bool on) {
        guard_native_mutation("set_broker_state_hash_recording");
        broker_state_hash_recording_ = on;
    }

    // Toggle volume-weighted per-sub-bar sampling inside run_magnified_bar.
    // Has no effect unless bar magnifier is enabled.
    void set_magnifier_volume_weighted(bool on) {
        guard_native_mutation("set_magnifier_volume_weighted");
        magnifier_volume_weighted_ = on;
    }

    // --- Runtime trace API ---
    // Default off so existing strategies pay zero cost. The validator
    // flips this on per-strategy via ``strategy_set_trace_enabled`` (the
    // FFI shim defined in c_abi.cpp) before running a backtest whose
    // per-bar values it wants to cross-reference against TradingView.
    void set_trace_enabled(bool on) {
        guard_native_mutation("set_trace_enabled");
        trace_enabled_ = on;
    }
    bool trace_enabled() const { return trace_enabled_; }

    // --- Live-runtime status API (ABI v4) ---
    int last_run_status() const { return last_run_status_; }

    // Request cooperative abort of the run in progress on this handle (a
    // live runtime supersedes an in-flight probe run). Safe to call from any
    // thread; consumed by the running loop at its next bar. A request made
    // while idle is cleared at the next run() entry and is a no-op.
    void request_abort() { abort_requested_.store(true, std::memory_order_relaxed); }

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

} // inline namespace engine_script_run_v18
} // namespace pineforge
