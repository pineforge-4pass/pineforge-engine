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

// Where an opened lot's own fill sits on its entry bar's modeled path — the
// entry-side half of the excursion capability above, and the only thing the
// owner of a lot's excursion has to say about it.  OnPath names a fill at a
// price the path reaches, so the kernel derives which end (if either) the
// path had already reached before it; AfterPath names a fill at the bar's
// closing point, after the whole path, so both ends precede it.  The geometry
// is the kernel's: which leg is walked first and where a price is first
// touched are the same rules the matcher uses, and a host that forces the leg
// order does it once, through NativeRunSpec::path_order.
enum class OpenedLotFillPoint : std::uint8_t {
    OnPath = 0,
    AfterPath = 1,
};

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
    // bar -- a report-only mark-to-market row
    // (NativeExecutionConsumer::append_open_position_report_rows); false for
    // every script-driven or bracket exit. Mirrors pf_trade_t::open_at_end.
    bool open_at_end = false;
    // Why this row exited, when the closer knew. A kernel-originated
    // liquidation or risk flatten carries its own cause through the settling
    // execution::Fill; a host that runs its own forced-close policy records
    // its cause on the row. Unspecified leaves closed_trade_close_cause() to
    // the generic facts above (open_at_end, exit_from_bracket).
    execution::CloseCause close_cause = execution::CloseCause::Unspecified;
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
// The fold broker_state_hash() is built on: FNV-1a over a canonical byte
// spelling of each value (-0.0 folds as 0.0, every NaN as one quiet NaN, a
// string as its length then its bytes). It is a public, complete type because
// a host writes into it: BacktestEngine::hash_host_extension receives the sink
// the kernel has already folded its own broker state into, and whatever the
// host folds after that is part of every scalar, per-bar and stream hash of
// the run. Begin an extension with a domain tag of your own (`sink.s(...)`),
// then fold each durable value in a fixed order.
class BrokerStateHashSink {
public:
    uint64_t h = 1469598103934665603ULL;

    void bytes(const void* p, size_t n) {
        const unsigned char* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ULL; }
    }

    void d(double v) {
        if (v == 0.0) v = 0.0;
        if (v != v) v = std::numeric_limits<double>::quiet_NaN();
        bytes(&v, sizeof v);
    }

    void i(int64_t v) { bytes(&v, sizeof v); }
    void u(uint64_t v) { bytes(&v, sizeof v); }
    void b(bool v) { const unsigned char c = v ? 1 : 0; bytes(&c, 1); }
    void s(const std::string& v) { u(v.size()); bytes(v.data(), v.size()); }
};
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
    // The host's own durable state, folded last into broker_state_hash():
    // called exactly once per hash, after the kernel's generic broker state.
    // A host whose next decision depends on state the kernel does not own — a
    // regime, a counter, a model — overrides this and folds it, under its own
    // domain tag, so a replay that diverges there diverges in the hash. The
    // override replaces the default; call BacktestEngine::hash_host_extension
    // first to keep the default's bytes and append to them. A host that
    // overrides nothing folds exactly what it always folded: the default
    // forwards to the deprecated spelling below, whose default is the
    // "source:none" marker.
    // @host-seam (native_c_api.h, COVERAGE / BASE-CLASS SEAMS)
    virtual void hash_host_extension(BrokerStateHashSink&) const;
    // Deprecated spelling of hash_host_extension, named by the source layer
    // before a bare host could extend the fold. Still folded when it is the
    // only one overridden, so an existing subclass compiles and hashes
    // unchanged; the kernel itself calls hash_host_extension only.
    // @host-seam (native_c_api.h, COVERAGE / BASE-CLASS SEAMS)
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

    int64_t trade_start_time_ = std::numeric_limits<int64_t>::min();


    // --- SymInfo + Input injection ---
    SymInfo syminfo_;
    int64_t last_bar_time_ = 0;
    int last_bar_index_ = 0;
    // Forced intrabar path order (ABI v4 live-runtime surface, task 4): 0
    // AUTO, 1 HIGH_FIRST, 2 LOW_FIRST. Any other value is clamped to AUTO by
    // set_path_order() -- this member is always one of {0,1,2}. Persistent
    // configuration -- reset_run_state() does not touch it. See
    // set_path_order(). No kernel path reads it: native-bound source hosts
    // project it into NativeRunSpec::path_order at begin, so the native
    // driver owns the active batch/stream path order.
    int path_order_mode_ = 0;
    // Retained last-array-bar visibility flag (waived from the fold). The
    // probe tail suppression it once gated is the source host's (see
    // set_probe_suppress_tail_logic below); no kernel path reads it.
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
    // Timestamp of the input bar that FOLLOWS the one being fed to the
    // request.security evaluators; 0 = unknown (streams, the feed's last
    // bar). A historical run holds its whole feed, and the calendar
    // aggregator uses the hint to finalize a D/W/M bucket on the period's
    // actual last chart bar -- early closes and exchange holidays included
    // (TimeframeAggregator::feed(bar, next_input_ms)). Set by whoever pumps
    // the evaluators, for each bar it feeds; never for a stream's live input.
    int64_t security_next_input_ms_ = 0;
    uint64_t next_order_incarnation_ = 1;
    // Transient companion for TRAIL exits: the trail's best (peak) price at
    // fill time. The peak that armed the trailing stop is by definition a
    // pre-fill favorable excursion of the closing trade (TV reports
    // MFE == fill + offset == peak), but first_touch_position can't place a
    // trail fill on the bar path (the level is only active after the peak),
    // so emit_close_trade folds the peak directly. NaN = not a trail fill.
    double fold_exit_trail_peak_ = std::numeric_limits<double>::quiet_NaN();
    std::vector<Trade> trades_;
    // Report-only rows for a position still open when the feed ends, produced
    // by NativeExecutionConsumer::record_open_position_report_rows when the
    // spec asks for them (NativeRunSpec::report_open_position_at_end). They
    // are merged behind trades_ by fill_trades_section and never enter
    // trades_, the realized sums, or the live position (a stream continues it).
    std::vector<Trade> range_end_trades_;

    // Best favorable price is updated by the physical open/add helpers for
    // every host. Source policy may consume the same value through
    // inheritance, while the public C observer uses the virtual projection.
    double trail_best_price_ = std::numeric_limits<double>::quiet_NaN();

    // Generic synchronous close obligation. Pine quota/cause/beneficiary
    // state remains exclusively in the compatibility facade above.
    broker::PositionCloseObligation position_close_obligation_;

    // --- Cached trade metrics (updated incrementally by record_close_trade) ---
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

    // @broker-state end

    // Host-installed excursion capability (RULING A48).  Transient run
    // wiring, not durable broker state: reset_run_state clears it and the
    // consumer reinstalls it once per run when the host declares ownership.
    LotExcursionHook lot_excursion_hook_;
    // The entry-side declaration of that capability: the owner of a lot's
    // excursion names where the lot's opening fill sits on its entry bar, and
    // the kernel derives the two entry-bar mask flags of every lot booked
    // under `entry_incarnation` from the bar's path, walked in the leg order
    // the run declares (NativeRunSpec::path_order, the order the matcher
    // walks).  A fill the path never reaches leaves that lot's flags as they
    // were.  The flags are durable lot state: they are folded into the run
    // hash and handed back to the owner on the closing row's
    // ClosedLotExcursionFacts.
    // @host-seam (native_c_api.h, COVERAGE / BASE-CLASS SEAMS)
    void declare_opened_lot_entry_bar_mask(uint64_t entry_incarnation,
                                           const Bar& entry_bar,
                                           OpenedLotFillPoint fill_point);
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
    // The same inspection at an explicit account-currency rate: the resulting
    // notional and the quoted charges convert at `fx` and no clock is read.
    // inspect_native_settlement_scoped() is this at the presented clock's rate.
    execution::SettlementInspection inspect_native_settlement_scoped_at(
        const execution::Action& action, const execution::Fill& fill,
        execution::CloseScope scope, double fx) const;
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
    // The same value at an explicit account-currency rate, and its only
    // implementation: marked_equity(price) is this at the presented clock's
    // rate, a margin check point calls it at its own cursor's rate.
    double marked_equity_at(double price, double fx) const;

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
    // Money is not rounded here at all: ten-significant-digit money is the
    // source adapter's own arithmetic, and the kernel's money is exact.
    double round_to_mintick(double price) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        return std::floor(price / syminfo_mintick_ + 0.5) * syminfo_mintick_;
    }

    // A fill taken AT A RAW BAR PRICE — a market order at the bar close
    // (process_orders_on_close) or at the next open, a resting stop/limit
    // the open gapped through, a stop-limit whose limit is already
    // marketable at an OHLC path point, a margin-call slice at the open or
    // adverse extreme, a strategy.close at the close / COOF bar-point
    // cursor — books the raw print rounded to the NEAREST tick and only then
    // carries slippage ticks. The FEED is never quantized (indicators consume
    // the raw sub-tick values); only the fill and the broker's default-sizing
    // snapshot (calc_qty / frozen_sizing_price, same nearest-tick form) are
    // on-tick. The directional snap
    // (native_matching::grid_round_directional, src/native_matching.hpp) is
    // reserved for COMPUTED stop / limit LEVELS that fall between ticks;
    // applying it to a raw bar price was the finding-432/446 defect (sells
    // floored, buys ceiled — 43 AAPL slugs off by one tick). The result is
    // on-tick, so the matcher's directional snap downstream is an identity
    // on it (its ladder-exactness test absorbs the n*tick/tick FP residue).
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

    // The basis of a default-sized request is the request's own: SizeTime says
    // WHEN it resolves (AtMatch / AtAcceptance) and SizePrice says WHICH price
    // it converts at (Resolved / Signal / SignalOnTick), so this class freezes
    // no sizing of its own (ADR-0001 rule 5).
    // When a forced liquidation is tested is the spec's NativeLiquidationCheck;
    // this class keeps no intrabar-liquidation rule of its own.
    // What a script-visible position accessor reads around a close is decided
    // by NativeCloseExecution, not by a freeze in this class.
    // --- Strategy variable accessors ---
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
    // When true, the intrabar path scales per-sub-bar sample count by
    // (sub_bar.volume / mean_sub_bar_volume) within each script bar — dense
    // tick approximation on high-volume sub-bars without real tick data.
    // The flag reaches the host as NativeBeginArgs::magnifier_volume_weighted;
    // a host that projects the magnifier into the run's IntrabarPath carries
    // it as that path's volume_weighted, which the execution consumer's
    // deliver_intrabar_script samples by (sample_price_path_volume_weighted).
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
    // so the exit fill evaluation runs its open-gap shortcut on the in-flight
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
    // Current-bar session predicates. The host that dispatches the bar sets
    // them (the source adapter through scheduler_set_session_bar_state); the
    // kernel only clears them, in reset_run_state().
    bool session_ismarket_ = false;
    bool session_isfirstbar_ = false;
    bool session_islastbar_ = false;

    // session.ismarket of the CHART bar stamped bar_ms on the symbol's
    // session clock — the chart-timeframe-aware rule of session_time.hpp:
    // every bar of a daily-or-higher chart is the regular-session bar,
    // intraday bars keep the time-of-day test.
    bool chart_bar_ismarket(int64_t bar_ms) const;

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
    struct SecurityEvalState {
        int sec_id = 0;
        std::string tf;
        TimeframeAggregator aggregator;
        Bar current_bar{};
        int current_sub_bar_count = 0;
        int64_t feed_count = 0;
        int64_t eval_complete_count = 0;
        int64_t eval_partial_count = 0;
        // Requested-context bar index of the latest dispatch_security_eval()
        // (the ring address its TA members saw, see ta::bar_context()); -1
        // before the first dispatch. A host that replays its latest
        // evaluation re-dispatches the same bar under the same index.
        int64_t ta_bar_index = -1;
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

    // Registers one aggregating evaluator: the requested timeframe's buckets
    // built from the run's evaluator input timeframe, passthrough when the
    // two are equal. Publication modes are not the kernel's: a source host
    // keeps its own per-sec_id semantics beside this state and a native
    // subscription's modes are the consumer's delivery rules.
    void register_security_eval(int sec_id, const std::string& requested_tf,
                                const std::string& input_tf);
    // The one path to evaluate_security(): installs the requested context's
    // bar index for the evaluator's TA members (ta::bar_context()) for the
    // duration of the dispatch. `bar_index` is the 0-based index of the
    // requested-context bar being evaluated — the just-completed bucket for a
    // complete evaluation (eval_complete_count - 1), the in-progress bucket
    // for a partial one (eval_complete_count) — so every
    // compute()/recompute() dispatch of one requested bar rewrites the same
    // ring slot, and a conditional window call inside the security expression
    // is addressed exactly like TradingView addresses it.
    void dispatch_security_eval(SecurityEvalState& state, const Bar& bar,
                                bool publish, int64_t bar_index);
    // The generic evaluator step for one input bar: aggregate, take the
    // native bar of a completed bucket where a feed serves the timeframe,
    // and dispatch the completed bucket. Publication at the last contributing
    // input is the consumer's; a source host composes its own step.
    void feed_security_eval_state(SecurityEvalState& state, const Bar& input_bar);

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
    // MUST mirror this trough-reset logic; keep in lockstep. The fold is one
    // per script calculation and the kernel performs it under EVERY report
    // policy (native_execution_consumer.cpp): these scalars measure the run,
    // not the report. A policy that also records the curve folds and appends
    // at one instant, so a re-walk of the curve through fold_equity_extreme
    // reproduces the scalars bit for bit (scheduler_record_range_end uses it).
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
        const execution::SelectedOpeningSet* selected, double fx) const;
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
        const execution::LifecycleEffects* lifecycle, double fx) const;
    void stage_native_settlement(
        NativeSettlementStage& stage,
        const execution::ReverseTo& reversal,
        const execution::Fill& fill,
        const execution::LifecycleEffects* lifecycle, double fx) const;
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
    // Quote one resolved execution's current charges, a percent schedule at
    // the account-currency rate `fx` its stage converts at. Entry costs on the
    // closed rows are historical allocations. Returns close shares in FIFO
    // order followed by the opening share (zero when there is no opening).
    std::vector<double> quote_execution_commissions(
        const std::vector<double>& closed_units, double opening_units,
        const execution::Fill& fill, double fx) const;
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
#endif

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

    virtual void set_syminfo_metadata(const std::string& key, double value);

    // Returns the script's active timeframe string (e.g. "15" for 15-minute,
    // "D" for daily). Backs timeframe.main_period in generated Pine v6 code.
    const std::string& main_period() const { return script_tf_; }

    // Host run-mode overrides behind the frozen C setters
    // strategy_set_realtime_tail / strategy_set_probe_suppress_tail_logic
    // (pineforge.h). The kernel keeps no forming-tail state of its own: a
    // batch is complete input and the forward path is the stream, so
    // "the last fed bar is still forming" and "run only the broker steps on
    // the last bar" are a host's live-probe protocol, not a kernel mode. A
    // host that models one (source::PineStrategyHost, for the live runner's
    // probe / settle cycle) overrides both, owns the state and answers true.
    // The kernel default keeps the C ingress contract these setters always
    // had on a bare host -- accepted before begin, guarded like every other
    // setter, and inert (no kernel path ever read the flags on a native
    // host) -- so it stores nothing, leaves last_error() untouched and
    // answers false, meaning "no such mode here". R5 lane N14: the flags
    // were BacktestEngine members until then.
    virtual bool set_realtime_tail(bool on, int horizon_bars);
    virtual bool set_probe_suppress_tail_logic(bool on);

    // Force this run's intrabar path order (ABI v4 live-runtime surface,
    // task 4): 0 AUTO (the unchanged |H-O| vs |O-L| rule), 1 HIGH_FIRST
    // (O -> H -> L -> C), 2 LOW_FIRST (O -> L -> H -> C). Any other value is
    // clamped to AUTO. A live probe runs the SAME forming bar under both
    // forced orders and keeps only the fills that agree between the two --
    // a fill that depends on which leg TradingView's own still-forming bar
    // will resolve to is path-dependent and must be suppressed rather than
    // guessed.
    // Persistent configuration, like set_realtime_tail -- stays set until a
    // caller passes mode=0. It only stores the mode: a native-bound source
    // provider projects it into NativeRunSpec::path_order at begin (a bare
    // native host declares that field itself), and the native driver walks
    // the spec field; the sampler's thread-local override
    // (internal::set_path_order_override) is installed by the consumer alone.
    // Default AUTO (mode=0): every historical run stays byte-identical to
    // before this flag existed.
    void set_path_order(int mode) {
        guard_native_mutation("set_path_order");
        path_order_mode_ = (mode == 1 || mode == 2) ? mode : 0;
    }

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
    // The live-runtime observation seam (ABI v4, spec 3.6): read-only views
    // of the arbitration this run's last bar decided, of the resting-order
    // book in force for the next bar and of the engine-computed derived
    // values of one resting order. Every one is a pure const read, so a run
    // is byte-identical whether or not a caller reads it. The kernel's own
    // answer is "not available" (0 / -1 / NaN): the host that owns the book
    // overrides them, and the C surface documents what each value means
    // (strategy_last_bar_dual_entry_path, strategy_pending_orders_len,
    // strategy_pending_order_get, strategy_pending_order_fill_qty,
    // strategy_pending_order_level_resolved,
    // strategy_pending_order_effective_levels, strategy_trail_best_price --
    // include/pineforge/pineforge.h).
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
    // report point appends broker_state_hash() to broker_state_hashes_
    // immediately after that bar's record_equity_point() call -- under
    // KernelRecorded the execution consumer's record_script_report_point,
    // which every script calculation it delivers reaches (confirmed,
    // intrabar and aggregated bars, in a batch run and across a stream's
    // warmup and realtime legs); a host that records or marks its own report
    // points appends its own row at each -- so the recorded array's length
    // matches script_bars_processed and pf_report_t::broker_state_hash_len
    // 1:1 -- including on strategy_stream_fill_report, whose report is the
    // cumulative warmup + realtime run. Default off: broker_state_hashes_
    // stays empty, fill_report emits a null/zero-length array, and every
    // historical run stays byte-identical to before this flag existed.
    // reset_run_state() clears the recorded array on every run() (the
    // warmup leg of stream_begin included) regardless of this flag's
    // value; the flag itself is persistent configuration, like
    // set_realtime_tail, so it must be set BEFORE stream_begin to also
    // cover the warmup bars.
    //
    // What a row is, and what it is for. A row is the run's CONTINUATION
    // IDENTITY at that bar, not its trade outcome: broker_state_hash() is
    // broker_state_hash_from_execution_hash(continuation_hash()), so it folds
    // the kernel's broker state AND, ahead of it, the state a resume would
    // continue from. NativeRunPhase (Batch/Warmup/Realtime, readable as
    // native_state().phase) is part of that continuation on purpose, because a
    // consumer mid-warmup and one mid-realtime are not interchangeable
    // continuations. So the array is a replay check WITHIN one driving mode
    // and deliberately not across modes:
    //   * same driving -- two runs driven the same way record the same rows,
    //     and the row after bar k is the last row of a run driven the same way
    //     that ended at bar k (a batch and a stream at any warmup split);
    //   * different driving -- run(), stream_begin(warmup=1)+push and
    //     stream_begin(warmup=all) over the same bars booking the same trades
    //     record different rows from index 0; only the length identity above
    //     survives. Two streams share exactly their common Warmup prefix.
    //   * the broker half alone IS driving-mode invariant: factor the
    //     continuation out with broker_state_hash_from_execution_hash(fixed)
    //     and the remaining fold is identical at every bar in every driving.
    // The batch<->stream oracle is therefore the OUTCOME, not this array:
    // tests/test_native_margin_fx_roll.cpp section 8 and tests/test_streaming.cpp
    // for the kernel, scripts/check_corpus_parity.sh for the Pine adapter.
    // Both directions are pinned in tests/test_native_report_truth.cpp.
    void set_broker_state_hash_recording(bool on) {
        guard_native_mutation("set_broker_state_hash_recording");
        broker_state_hash_recording_ = on;
    }

    // Toggle volume-weighted per-sub-bar sampling of the intrabar path (see
    // magnifier_volume_weighted_). Has no effect unless bar magnifier is
    // enabled.
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
