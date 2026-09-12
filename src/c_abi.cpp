/*
 * c_abi.cpp — runtime-side implementation of the public C ABI declared
 * in <pineforge/pineforge.h>.
 *
 * Contains:
 *   - Layout-compatibility static_asserts between the public C PODs
 *     and the internal C++ types they mirror. If any of these trip,
 *     the C ABI has drifted from the internal representation — fix
 *     BEFORE shipping a .so that consumers depend on.
 *   - The runtime-library-side `extern "C"` symbols (the closed-trade
 *     incarnation accessor, setters, strategy_get_last_error,
 *     the auxiliary-security-feed setter, the strategy_stream_* lifecycle,
 *     native stream bar input/action polling/fingerprint/API-version exports,
 *     the live-runtime surface (strategy_request_abort,
 *     strategy_last_run_status, strategy_set_realtime_tail,
 *     strategy_set_probe_suppress_tail_logic, strategy_set_path_order,
 *     strategy_last_bar_dual_entry_path,
 *     strategy_set_broker_state_hash_recording, strategy_broker_state_hash,
 *     strategy_pending_orders_len, strategy_pending_order_get,
 *     strategy_pending_order_layout, strategy_pending_order_fill_qty,
 *     strategy_pending_order_level_resolved,
 *     strategy_pending_order_effective_levels, strategy_trail_best_price,
 *     strategy_position_avg_price, strategy_position_cycle_seq,
 *     strategy_closed_trade_entry_id, strategy_closed_trade_exit_id,
 *     strategy_closed_trade_exit_comment, strategy_closed_trade_close_cause,
 *     strategy_position_size, strategy_current_equity,
 *     strategy_script_bars_processed),
 *     pf_version_get/pf_version_string,
 *     pf_abi_version, strategy_execution_contract,
 *     strategy_configure_native_v1 — the authoritative list is EXPECTED_RUNTIME in
 *     scripts/check_c_abi_runtime.py, enforced by CI). The other
 *     `extern "C"` symbols listed in pineforge.h (strategy_create,
 *     run_backtest, etc.) are emitted per-compiled-strategy by the
 *     codegen, not here.
 */

// Include order is load-bearing: pineforge.h BEFORE engine.hpp keeps the
// per-strategy declarations visible so definitions here are prototype-checked.
// engine.hpp defines PINEFORGE_NO_STRATEGY_DECLS, which suppresses them.
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/bar.hpp>
#include <pineforge/magnifier.hpp>
#include <cstddef>
#include <limits>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace {

template <typename Fn>
void pf_cabi_void(Fn&& fn) noexcept {
    try {
        std::forward<Fn>(fn)();
    } catch (...) {
    }
}

template <typename Fn>
int pf_cabi_int(Fn&& fn) noexcept {
    try {
        return std::forward<Fn>(fn)();
    } catch (...) {
        return -1;
    }
}

}  // namespace

namespace pineforge {
// Generated (src/pending_order_mirror.cpp, scripts/gen_pending_order_mirror.py).
void fill_pending_order_mirror(const PendingOrder& src,
                               const MarketAdmissionJournal* journal,
                               pf_pending_order_v1_t* out);
const pf_field_desc_t* pending_order_layout(int* count);
}  // namespace pineforge

/* ── Bar layout parity ──────────────────────────────────────────── */

static_assert(sizeof(pf_bar_t) == sizeof(pineforge::Bar),
              "pf_bar_t / pineforge::Bar size mismatch");
static_assert(offsetof(pf_bar_t, open) == offsetof(pineforge::Bar, open),
              "pf_bar_t::open offset mismatch");
static_assert(offsetof(pf_bar_t, high) == offsetof(pineforge::Bar, high),
              "pf_bar_t::high offset mismatch");
static_assert(offsetof(pf_bar_t, low) == offsetof(pineforge::Bar, low),
              "pf_bar_t::low offset mismatch");
static_assert(offsetof(pf_bar_t, close) == offsetof(pineforge::Bar, close),
              "pf_bar_t::close offset mismatch");
static_assert(offsetof(pf_bar_t, volume) == offsetof(pineforge::Bar, volume),
              "pf_bar_t::volume offset mismatch");
static_assert(offsetof(pf_bar_t, timestamp) == offsetof(pineforge::Bar, timestamp),
              "pf_bar_t::timestamp offset mismatch");

/* ── Normalized trade-tick layout parity ───────────────────────── */

static_assert(sizeof(pf_trade_tick_t) == sizeof(pineforge::TradeTick),
              "pf_trade_tick_t / pineforge::TradeTick size mismatch");
static_assert(offsetof(pf_trade_tick_t, timestamp)
                  == offsetof(pineforge::TradeTick, timestamp),
              "pf_trade_tick_t::timestamp offset mismatch");
static_assert(offsetof(pf_trade_tick_t, sequence)
                  == offsetof(pineforge::TradeTick, sequence),
              "pf_trade_tick_t::sequence offset mismatch");
static_assert(offsetof(pf_trade_tick_t, price)
                  == offsetof(pineforge::TradeTick, price),
              "pf_trade_tick_t::price offset mismatch");
static_assert(offsetof(pf_trade_tick_t, quantity)
                  == offsetof(pineforge::TradeTick, quantity),
              "pf_trade_tick_t::quantity offset mismatch");

/* ── Trade layout parity ────────────────────────────────────────── */

static_assert(sizeof(pf_trade_t) == sizeof(pineforge::TradeC),
              "pf_trade_t / pineforge::TradeC size mismatch");
static_assert(offsetof(pf_trade_t, entry_time) == offsetof(pineforge::TradeC, entry_time),
              "pf_trade_t::entry_time offset mismatch");
static_assert(offsetof(pf_trade_t, exit_time) == offsetof(pineforge::TradeC, exit_time),
              "pf_trade_t::exit_time offset mismatch");
static_assert(offsetof(pf_trade_t, entry_price) == offsetof(pineforge::TradeC, entry_price),
              "pf_trade_t::entry_price offset mismatch");
static_assert(offsetof(pf_trade_t, qty) == offsetof(pineforge::TradeC, qty),
              "pf_trade_t::qty offset mismatch");
static_assert(offsetof(pf_trade_t, commission) == offsetof(pineforge::TradeC, commission),
              "pf_trade_t::commission offset mismatch");
static_assert(offsetof(pf_trade_t, entry_bar_index) == offsetof(pineforge::TradeC, entry_bar_index),
              "pf_trade_t::entry_bar_index offset mismatch");
static_assert(offsetof(pf_trade_t, exit_bar_index) == offsetof(pineforge::TradeC, exit_bar_index),
              "pf_trade_t::exit_bar_index offset mismatch");
static_assert(offsetof(pf_trade_t, open_at_end) == offsetof(pineforge::TradeC, open_at_end),
              "pf_trade_t::open_at_end offset mismatch");

/* ── SecurityDiag layout parity ─────────────────────────────────── */
/* The middle two fields differ in name (complete_count / partial_count
 * vs the internal C++ names) but layout is identical. Size + first +
 * last field offsets cover the whole struct. */

static_assert(sizeof(pf_security_diag_t) == sizeof(pineforge::SecurityDiagC),
              "pf_security_diag_t / pineforge::SecurityDiagC size mismatch");
static_assert(offsetof(pf_security_diag_t, sec_id) == offsetof(pineforge::SecurityDiagC, sec_id),
              "pf_security_diag_t::sec_id offset mismatch");
static_assert(offsetof(pf_security_diag_t, feed_count) == offsetof(pineforge::SecurityDiagC, feed_count),
              "pf_security_diag_t::feed_count offset mismatch");

/* ── TraceEntry layout parity ───────────────────────────────────── */

static_assert(sizeof(pf_trace_entry_t) == sizeof(pineforge::TraceEntryC),
              "pf_trace_entry_t / pineforge::TraceEntryC size mismatch");
static_assert(offsetof(pf_trace_entry_t, timestamp) == offsetof(pineforge::TraceEntryC, timestamp),
              "pf_trace_entry_t::timestamp offset mismatch");
static_assert(offsetof(pf_trace_entry_t, value) == offsetof(pineforge::TraceEntryC, value),
              "pf_trace_entry_t::value offset mismatch");

/* ── Report layout parity ───────────────────────────────────────── */

static_assert(sizeof(pf_report_t) == sizeof(pineforge::ReportC),
              "pf_report_t / pineforge::ReportC size mismatch");
static_assert(offsetof(pf_report_t, total_trades) == offsetof(pineforge::ReportC, total_trades),
              "pf_report_t::total_trades offset mismatch");
static_assert(offsetof(pf_report_t, trades) == offsetof(pineforge::ReportC, trades),
              "pf_report_t::trades offset mismatch");
static_assert(offsetof(pf_report_t, net_profit) == offsetof(pineforge::ReportC, net_profit),
              "pf_report_t::net_profit offset mismatch");
static_assert(offsetof(pf_report_t, security_diag) == offsetof(pineforge::ReportC, security_diag),
              "pf_report_t::security_diag offset mismatch");
static_assert(offsetof(pf_report_t, trace_names_len) == offsetof(pineforge::ReportC, trace_names_len),
              "pf_report_t::trace_names_len tail offset mismatch");
static_assert(offsetof(pf_report_t, metrics) == offsetof(pineforge::ReportC, metrics),
              "pf_report_t::metrics offset mismatch");
static_assert(offsetof(pf_report_t, equity_curve) == offsetof(pineforge::ReportC, equity_curve),
              "pf_report_t::equity_curve offset mismatch");
static_assert(offsetof(pf_report_t, equity_curve_len) == offsetof(pineforge::ReportC, equity_curve_len),
              "pf_report_t::equity_curve_len offset mismatch");
static_assert(offsetof(pf_report_t, broker_state_hash) == offsetof(pineforge::ReportC, broker_state_hash),
              "pf_report_t::broker_state_hash offset mismatch");
static_assert(offsetof(pf_report_t, broker_state_hash_len) == offsetof(pineforge::ReportC, broker_state_hash_len),
              "pf_report_t::broker_state_hash_len offset mismatch");

/* ── Magnifier distribution enum parity ─────────────────────────── */

static_assert(static_cast<int>(pineforge::MagnifierDistribution::UNIFORM)      == PF_MAGNIFIER_UNIFORM,
              "MagnifierDistribution::UNIFORM enum value mismatch");
static_assert(static_cast<int>(pineforge::MagnifierDistribution::COSINE)       == PF_MAGNIFIER_COSINE,
              "MagnifierDistribution::COSINE enum value mismatch");
static_assert(static_cast<int>(pineforge::MagnifierDistribution::TRIANGLE)     == PF_MAGNIFIER_TRIANGLE,
              "MagnifierDistribution::TRIANGLE enum value mismatch");
static_assert(static_cast<int>(pineforge::MagnifierDistribution::ENDPOINTS)    == PF_MAGNIFIER_ENDPOINTS,
              "MagnifierDistribution::ENDPOINTS enum value mismatch");
static_assert(static_cast<int>(pineforge::MagnifierDistribution::FRONT_LOADED) == PF_MAGNIFIER_FRONT_LOADED,
              "MagnifierDistribution::FRONT_LOADED enum value mismatch");
static_assert(static_cast<int>(pineforge::MagnifierDistribution::BACK_LOADED)  == PF_MAGNIFIER_BACK_LOADED,
              "MagnifierDistribution::BACK_LOADED enum value mismatch");

/* ───────────────────────────────────────────────────────────────────
 * Runtime-library-side extern "C" implementations.
 *
 * These live in libpineforge.a (statically linked into every compiled
 * strategy .so), so consumers find them via dlsym on any .so. The
 * other extern "C" symbols listed in pineforge.h are emitted PER
 * STRATEGY by the codegen (in emit_top.py::_emit_extern_c).
 * ─────────────────────────────────────────────────────────────────── */

extern "C" {

/* trade_index is a REPORT row index: pf_report_t::trades is trades_ followed
 * by range_end_trades_ (fill_trades_section, engine_report.cpp), so the
 * accessor indexes that same row space (report_trade_count /
 * get_report_trade).
 *
 * round-4b F3: this bounded the index by trade_count() == trades_.size(),
 * so every range-end row (the position still open after the final bar,
 * ABI v3's open_at_end) read as out of range and returned 0 — an empty
 * "Engine entry incarnation" on the run_strategy.py CSV. The grader then
 * failed closed on the identity gate and the verifier ladder rejected the
 * TV-identical trim candidate for ena-grid (XAUUSD 1D), even though the
 * engine's per-lot range-end rows themselves matched TV (xau-grid 6/6,
 * silicon 9/9 rows). The row is built by build_close_trade from the open
 * pyramid lot, so it carries that lot's entry_incarnation like any other
 * close. No struct changed: PF_ABI_VERSION stays 3. */
PF_API uint64_t strategy_closed_trade_entry_incarnation(
        pf_strategy_t s, int trade_index) {
    if (!s) return 0;
    const auto* engine = static_cast<const pineforge::BacktestEngine*>(s);
    if (trade_index < 0 || trade_index >= engine->report_trade_count()) return 0;
    return engine->get_report_trade(trade_index).entry_incarnation;
}

/* ABI v4 live-runtime surface (task 9): closed-trade id / exit-comment
 * string accessors, indexing the same REPORT row space as
 * strategy_closed_trade_entry_incarnation above (trades_ then
 * range_end_trades_). These read Trade::entry_id / exit_id / exit_comment
 * directly rather than through the protected BacktestEngine::closed_trade_
 * entry_id / _exit_id / _exit_comment methods -- those are a DIFFERENT,
 * narrower accessor (strategy.closedtrades.* scope: trades_ only, no
 * range-end rows) already declared with these exact names, so a same-name
 * report-row overload is not possible.
 *
 * Returned pointers are valid until the next run() (or stream call) on this
 * handle, like strategy_get_last_error -- the runtime's own std::string
 * storage backing them is untouched until then. NULL on a NULL handle or an
 * out-of-range trade_index. */
PF_API const char* strategy_closed_trade_entry_id(pf_strategy_t s, int trade_index) {
    if (!s) return nullptr;
    const auto* engine = static_cast<const pineforge::BacktestEngine*>(s);
    if (trade_index < 0 || trade_index >= engine->report_trade_count()) return nullptr;
    return engine->get_report_trade(trade_index).entry_id.c_str();
}

PF_API const char* strategy_closed_trade_exit_id(pf_strategy_t s, int trade_index) {
    if (!s) return nullptr;
    const auto* engine = static_cast<const pineforge::BacktestEngine*>(s);
    if (trade_index < 0 || trade_index >= engine->report_trade_count()) return nullptr;
    return engine->get_report_trade(trade_index).exit_id.c_str();
}

PF_API const char* strategy_closed_trade_exit_comment(pf_strategy_t s, int trade_index) {
    if (!s) return nullptr;
    const auto* engine = static_cast<const pineforge::BacktestEngine*>(s);
    if (trade_index < 0 || trade_index >= engine->report_trade_count()) return nullptr;
    return engine->get_report_trade(trade_index).exit_comment.c_str();
}

/* ABI v4 live-runtime surface (task 9): why a REPORT-row closed trade
 * exited (BacktestEngine::closed_trade_close_cause, engine_trade_
 * accessors.cpp, has the full derivation order). 0 UNKNOWN (reserved for
 * the documented "no cause" value on a VALID trade -- no live derivation
 * currently produces it), 1 SCRIPT (strategy.close/close_all or a
 * reversal-driven close), 2 BRACKET (a strategy.exit stop/limit/trail/
 * profit/loss leg), 3 MARGIN_CALL, 4 INTRADAY_LOSS_CAP, 5
 * INTRADAY_FILL_CAP, 6 RANGE_END (the still-open position closed at the
 * end of a flag-off run). -1 (final review F7) on a NULL handle OR an
 * out-of-range trade_index -- delegated to the engine method for the
 * latter, matching every sibling indexed accessor's -1-on-bad-index
 * convention (strategy_pending_order_fill_qty/_level_resolved/
 * _effective_levels). */
PF_API int strategy_closed_trade_close_cause(pf_strategy_t s, int trade_index) {
    if (!s) return -1;
    return static_cast<const pineforge::BacktestEngine*>(s)->closed_trade_close_cause(trade_index);
}

/* ABI v4 live-runtime surface (task 9): the script-facing position size
 * (strategy.position_size -- signed, KI-64 freeze-aware) and equity after
 * the most recent run(). strategy_current_equity is initial capital plus
 * realized net profit (strategy.initial_capital + strategy.netprofit) --
 * NOT Pine's strategy.equity, which adds open profit on top of this. NaN
 * on a NULL handle. */
PF_API double strategy_position_size(pf_strategy_t s) {
    if (!s) return std::numeric_limits<double>::quiet_NaN();
    return static_cast<const pineforge::BacktestEngine*>(s)->live_position_size();
}

PF_API double strategy_current_equity(pf_strategy_t s) {
    if (!s) return std::numeric_limits<double>::quiet_NaN();
    return static_cast<const pineforge::BacktestEngine*>(s)->live_current_equity();
}

/* ABI v4 live-runtime surface (task 9): total SCRIPT bars dispatched by the
 * most recent run() (mirrors pf_report_t::script_bars_processed, including
 * a stream's warmup leg and every realtime tick-driven bar dispatched
 * afterward). -1 on a NULL handle. */
PF_API int64_t strategy_script_bars_processed(pf_strategy_t s) {
    if (!s) return -1;
    return static_cast<const pineforge::BacktestEngine*>(s)->script_bars_processed();
}

/* Toggle per-bar trace recording on a live strategy. Default off; the
 * harness flips it on per-strategy via this entry point before running
 * a backtest whose per-bar values it wants to cross-reference against
 * TradingView. */
PF_API void strategy_set_trace_enabled(pf_strategy_t s, int on) {
    pf_cabi_void([&] {
        if (!s) return;
        static_cast<pineforge::BacktestEngine*>(s)->set_trace_enabled(on != 0);
    });
}

/* Returns the error message captured by the most recent run() on this
 * strategy, or an empty string if the run completed normally. The
 * pointer remains valid until the next run() (which clears the
 * captured error before it begins). Returns NULL only when ``s`` is
 * NULL. The runtime catches all std::exception derivatives inside
 * BacktestEngine::run() so the C ABI never unwinds an exception across
 * the extern "C" boundary; consumers must check this after every run
 * to surface engine-rejected configurations (e.g. script_tf finer than
 * input_tf, request.security TF below the chart TF without a supported
 * lower-TF emulation, missing input_tf when securities are registered).
 */
PF_API const char* strategy_get_last_error(pf_strategy_t s) {
    if (!s) return nullptr;
    return static_cast<pineforge::BacktestEngine*>(s)->last_error().c_str();
}

PF_API void strategy_set_trade_start_time(pf_strategy_t s, int64_t timestamp_ms) {
    pf_cabi_void([&] {
        if (!s) return;
        static_cast<pineforge::BacktestEngine*>(s)->set_trade_start_time(timestamp_ms);
    });
}

/* Cooperative abort of the run in progress on ``s`` (live runtime: a settle
 * supersedes an in-flight probe). Atomic; consumed by the running loop at its
 * next bar. Cleared once at every public run() entry so a request made
 * while idle is a no-op. The aborted run reports strategy_last_run_status()
 * == 1 and leaves the handle reusable. */
PF_API void strategy_request_abort(pf_strategy_t s) {
    if (!s) return;
    static_cast<pineforge::BacktestEngine*>(s)->request_abort();
}

/* 0 when the most recent run() completed, 1 when it was aborted
 * (NOT_COMPLETED), -1 when ``s`` is NULL. Errors are reported by
 * strategy_get_last_error. */
PF_API int strategy_last_run_status(pf_strategy_t s) {
    if (!s) return -1;
    return static_cast<const pineforge::BacktestEngine*>(s)->last_run_status();
}

/* Live-runtime tail semantics (spec §3.1): the last bar of the array fed to
 * the next run() is a still-forming bar, not the chart's rightmost
 * historical bar. Default off (on=0): every historical run stays
 * byte-identical to before this flag existed. */
PF_API void strategy_set_realtime_tail(pf_strategy_t s, int on, int horizon_bars) {
    pf_cabi_void([&] {
        if (!s) return;
        static_cast<pineforge::BacktestEngine*>(s)->set_realtime_tail(on != 0, horizon_bars);
    });
}

/* Live probe tail suppression (spec §3.2): the last bar of the array fed to
 * the next run() runs only dispatch_bar()'s pre-on_bar broker steps
 * (intraday-cap deferred close, source-series push, resting-order fills,
 * max-intraday-loss path check, per-trade extreme update) and returns —
 * on_bar is never invoked for that bar, and nothing after it runs (no
 * flush_same_bar_close, no POOC second pass, no process_margin_call, no
 * settle_dormant_bracket_reissues, no sizing refresh). Margin-call /
 * intraday-cap closes therefore surface only at settlement, not against the
 * still-forming probe bar. Independent of strategy_set_realtime_tail.
 * Honoured only on the standard dispatch_bar path; no-op under COOF and the
 * bar magnifier (gated in v1); undefined on input_tf < script_tf until the
 * partial-bucket flag lands -- see pineforge.h.
 * Default off (on=0): every historical run stays byte-identical to before
 * this flag existed. */
PF_API void strategy_set_probe_suppress_tail_logic(pf_strategy_t s, int on) {
    pf_cabi_void([&] {
        if (!s) return;
        static_cast<pineforge::BacktestEngine*>(s)->set_probe_suppress_tail_logic(on != 0);
    });
}

/* ABI v4 live-runtime surface (task 4): force the intrabar path order used
 * by every subsequent run() -- 0 AUTO (the unchanged TV-emulator rule), 1
 * HIGH_FIRST (O -> H -> L -> C), 2 LOW_FIRST (O -> L -> H -> C); any other
 * @p mode is clamped to AUTO. A live probe runs the SAME forming bar under
 * both forced orders and keeps only the fills that agree between the two.
 * Persistent configuration, like strategy_set_realtime_tail -- applies to
 * run() only, a stream continued via strategy_stream_begin always sees
 * AUTO. Default AUTO (mode=0): every historical run stays byte-identical to
 * before this flag existed. */
PF_API void strategy_set_path_order(pf_strategy_t s, int mode) {
    pf_cabi_void([&] {
        if (!s) return;
        static_cast<pineforge::BacktestEngine*>(s)->set_path_order(mode);
    });
}

/* ABI v4 live-runtime surface (task 4): the dual-entry-stop arbitration
 * decided on the LAST bar the most recent run() dispatched -- a flat
 * position resting one long stop-only ENTRY and one short stop-only ENTRY,
 * both touched on that bar. 0 = None (no such pair was arbitrated on that
 * bar), 1 = LongFirst, 2 = ShortFirst; -1 when @p s is NULL. This is a
 * per-bar snapshot: it survives a later fill or a declined stop-entry
 * admission on the same bar (both of which move the engine's own working
 * arbitration state back to None), so it reports the real decision even for
 * an ordinary process_orders_on_close run with no tail suppression. Only
 * the standard (non-calc_on_order_fills) dispatch path updates this. */
PF_API int strategy_last_bar_dual_entry_path(pf_strategy_t s) {
    if (!s) return -1;
    return static_cast<const pineforge::BacktestEngine*>(s)->last_bar_dual_entry_path();
}

/* ABI v4 live-runtime surface (task 6): toggle per-script-bar broker-state
 * hash recording. Default off: pf_report_t::broker_state_hash stays
 * NULL/0-length and every historical run is byte-identical to before this
 * flag existed. */
PF_API void strategy_set_broker_state_hash_recording(pf_strategy_t s, int on) {
    pf_cabi_void([&] {
        if (!s) return;
        static_cast<pineforge::BacktestEngine*>(s)->set_broker_state_hash_recording(on != 0);
    });
}

/* ABI v4 live-runtime surface (task 6): the broker-state hash of the FINAL
 * state after the most recent run(), regardless of whether per-bar
 * recording was enabled. Returns 0 when @p s is NULL. */
PF_API uint64_t strategy_broker_state_hash(pf_strategy_t s) {
    if (!s) return 0;
    return static_cast<const pineforge::BacktestEngine*>(s)->broker_state_hash();
}

/* ABI v4 live-runtime surface (task 7, spec 3.6): the resting-order book
 * after the most recent run(), read through the generated POD mirror
 * (pf_pending_order_v1_t, include/pineforge/pending_order_mirror.hpp;
 * fill_pending_order_mirror / pending_order_layout live in the generated
 * src/pending_order_mirror.cpp). Read-only accessors: no historical run
 * changes because a caller read them. */
PF_API int strategy_pending_orders_len(pf_strategy_t s) {
    if (!s) return 0;
    return static_cast<const pineforge::BacktestEngine*>(s)->pending_order_count();
}

/* Copies min(size_in, sizeof(pf_pending_order_v1_t)) bytes so an older
 * (smaller) or newer (larger) caller struct both work: the first two
 * fields are always struct_version and size. -1 (nothing written) on a
 * NULL handle/out, an out-of-range index, or a size_in too small to hold
 * even that 8-byte header -- a buffer that cannot receive struct_version
 * and size cannot be interpreted by any reader, so it is rejected rather
 * than partially filled. */
PF_API int strategy_pending_order_get(pf_strategy_t s, int index, void* out, size_t size_in) {
    if (!s || !out) return -1;
    if (size_in < offsetof(pf_pending_order_v1_t, size) + sizeof(uint32_t)) return -1;
    const auto* engine = static_cast<const pineforge::BacktestEngine*>(s);
    if (index < 0 || index >= engine->pending_order_count()) return -1;
    pf_pending_order_v1_t tmp;
    pineforge::fill_pending_order_mirror(engine->pending_order_at(index),
        &engine->market_admission_journal(), &tmp);
    std::memcpy(out, &tmp, size_in < sizeof(tmp) ? size_in : sizeof(tmp));
    return 0;
}

PF_API const pf_field_desc_t* strategy_pending_order_layout(int* count) {
    return pineforge::pending_order_layout(count);
}

/* ABI v4 live-runtime surface (task 8, spec 3.6): engine-computed derived
 * order values and position scalars -- pure const reads of the engine's own
 * sizing / admission / level-resolution predicates
 * (BacktestEngine::probe_fill_qty & co., src/engine_fills.cpp). NULL-handle
 * convention of the pf_live group: -1 for an int return, NaN for a double,
 * with nothing written through the out-pointers. */
PF_API int strategy_pending_order_fill_qty(pf_strategy_t s, int index, double fill_price,
                                           double* qty, int* close_only, int* partition) {
    if (!s) return -1;
    return static_cast<const pineforge::BacktestEngine*>(s)->probe_fill_qty(
        index, fill_price, qty, close_only, partition);
}

PF_API int strategy_pending_order_level_resolved(pf_strategy_t s, int index) {
    if (!s) return -1;
    return static_cast<const pineforge::BacktestEngine*>(s)->pending_order_level_resolved(index);
}

PF_API int strategy_pending_order_effective_levels(pf_strategy_t s, int index, double* stop,
                                                   double* limit, double* trail_activation) {
    if (!s) return -1;
    return static_cast<const pineforge::BacktestEngine*>(s)->pending_order_effective_levels(
        index, stop, limit, trail_activation);
}

/* NaN when @p s is NULL; otherwise the engine's trail extreme (itself NaN
 * until a position has filled). */
PF_API double strategy_trail_best_price(pf_strategy_t s) {
    if (!s) return std::numeric_limits<double>::quiet_NaN();
    return static_cast<const pineforge::BacktestEngine*>(s)->trail_best_price();
}

/* NaN when @p s is NULL or the position is flat (the engine keeps
 * position_entry_price_ at 0 there; a live reader must not mistake that for
 * a price), otherwise the volume-weighted average entry price. */
PF_API double strategy_position_avg_price(pf_strategy_t s) {
    if (!s) return std::numeric_limits<double>::quiet_NaN();
    const auto* engine = static_cast<const pineforge::BacktestEngine*>(s);
    if (engine->position_cycle_seq() == 0) return std::numeric_limits<double>::quiet_NaN();
    return engine->position_avg_price();
}

/* -1 when @p s is NULL; 0 when flat; otherwise the live position cycle id. */
PF_API int64_t strategy_position_cycle_seq(pf_strategy_t s) {
    if (!s) return -1;
    return static_cast<const pineforge::BacktestEngine*>(s)->position_cycle_seq();
}

PF_API int strategy_stream_begin(pf_strategy_t s,
                                 const pf_bar_t* warmup_bars,
                                 int n_warmup,
                                 const char* input_tf,
                                 const char* script_tf) {
    return pf_cabi_int([&] {
        if (!s) return -1;
        auto* engine = static_cast<pineforge::BacktestEngine*>(s);
        if (n_warmup < 0 || (n_warmup > 0 && !warmup_bars)) {
            // Preserve the engine's diagnostic and lifecycle contract without
            // reading malformed C input or allocating an input array.
            return engine->stream_begin(nullptr, n_warmup,
                input_tf ? std::string(input_tf) : std::string(),
                script_tf ? std::string(script_tf) : std::string()) ? 0 : -1;
        }
        std::vector<pineforge::Bar> bars;
        bars.reserve(static_cast<std::size_t>(n_warmup));
        for (int i = 0; i < n_warmup; ++i) {
            const auto& b = warmup_bars[i];
            bars.push_back({b.open, b.high, b.low, b.close, b.volume, b.timestamp});
        }
        return engine->stream_begin(
            bars.data(), n_warmup,
            input_tf ? std::string(input_tf) : std::string(),
            script_tf ? std::string(script_tf) : std::string()) ? 0 : -1;
    });
}

PF_API int strategy_stream_api_version(void) { return 1; }

PF_API int strategy_stream_push_bar(pf_strategy_t s, const pf_bar_t* bar) {
    return pf_cabi_int([&] {
        if (!s || !bar) return -1;
        const pineforge::Bar native{bar->open, bar->high, bar->low,
                                     bar->close, bar->volume, bar->timestamp};
        return static_cast<pineforge::BacktestEngine*>(s)->stream_push_bar(native) ? 0 : -1;
    });
}

PF_API int strategy_stream_order_actions_len(pf_strategy_t s) {
    if (!s) return -1;
    return static_cast<const pineforge::BacktestEngine*>(s)->stream_order_actions_len();
}

PF_API int strategy_stream_order_action_get(pf_strategy_t s, int index,
                                            pf_stream_order_action_t* out) {
    if (!s || !out || index < 0) return -1;
    const auto* engine = static_cast<const pineforge::BacktestEngine*>(s);
    if (index >= engine->stream_order_actions_len()) return -1;
    const auto& a = engine->stream_order_action_at(index);
    *out = pf_stream_order_action_t{a.sequence, a.timestamp_ms, a.bar_index,
        a.is_entry ? 1 : 0, a.is_long ? 1 : 0, a.quantity, a.price,
        a.order_id.c_str(), a.comment.c_str(), a.entry_incarnation};
    return 0;
}

PF_API void strategy_stream_order_actions_clear(pf_strategy_t s) {
    if (s) static_cast<pineforge::BacktestEngine*>(s)->stream_order_actions_clear();
}

PF_API uint64_t strategy_stream_state_hash(pf_strategy_t s) {
    if (!s) return 0;
    return static_cast<const pineforge::BacktestEngine*>(s)->stream_state_hash();
}

PF_API int strategy_stream_push_tick(pf_strategy_t s,
                                     const pf_trade_tick_t* tick) {
    return pf_cabi_int([&] {
        if (!s || !tick) return -1;
        const pineforge::TradeTick native{tick->timestamp, tick->sequence,
                                           tick->price, tick->quantity};
        return static_cast<pineforge::BacktestEngine*>(s)->stream_push_tick(native)
            ? 0
            : -1;
    });
}

PF_API int strategy_stream_push_ticks(pf_strategy_t s,
                                      const pf_trade_tick_t* ticks,
                                      int n) {
    return pf_cabi_int([&] {
        if (!s || n < 0 || (n > 0 && !ticks)) return -1;
        auto* engine = static_cast<pineforge::BacktestEngine*>(s);
        std::vector<pineforge::TradeTick> native;
        native.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const auto& tick = ticks[i];
            native.push_back({tick.timestamp, tick.sequence, tick.price, tick.quantity});
        }
        return engine->stream_push_ticks(native.data(), n) ? 0 : -1;
    });
}

PF_API int strategy_stream_advance_time(pf_strategy_t s, int64_t timestamp_ms) {
    return pf_cabi_int([&] {
        if (!s) return -1;
        return static_cast<pineforge::BacktestEngine*>(s)
            ->stream_advance_time(timestamp_ms) ? 0 : -1;
    });
}

PF_API int strategy_stream_end(pf_strategy_t s, int finalize_partial_input_bar) {
    return pf_cabi_int([&] {
        if (!s) return -1;
        return static_cast<pineforge::BacktestEngine*>(s)
            ->stream_end(finalize_partial_input_bar != 0) ? 0 : -1;
    });
}

PF_API int strategy_stream_fill_report(pf_strategy_t s, pf_report_t* out) {
    return pf_cabi_int([&] {
        if (!s || !out) return -1;
        static_cast<pineforge::BacktestEngine*>(s)->fill_report(
            reinterpret_cast<pineforge::ReportC*>(out));
        return 0;
    });
}

/* Override the chart TZ for ``hour``/``minute``/``dayofweek``/etc. See
 * pineforge.h docstring; NULL or empty are normalised to the legacy UTC
 * fast path. */
PF_API void strategy_set_chart_timezone(pf_strategy_t s, const char* tz) {
    pf_cabi_void([&] {
        if (!s) return;
        static_cast<pineforge::BacktestEngine*>(s)->set_chart_timezone(
            tz ? std::string(tz) : std::string());
    });
}

/* Plumb the symbol's exchange timezone / session string from the data feed
 * into syminfo_ (feeds session.ismarket / time(session)). NULL is ignored. */
PF_API void strategy_set_syminfo_timezone(pf_strategy_t s, const char* tz) {
    pf_cabi_void([&] {
        if (!s || !tz) return;
        static_cast<pineforge::BacktestEngine*>(s)->set_syminfo_timezone(std::string(tz));
    });
}

PF_API void strategy_set_syminfo_session(pf_strategy_t s, const char* session) {
    pf_cabi_void([&] {
        if (!s || !session) return;
        static_cast<pineforge::BacktestEngine*>(s)->set_syminfo_session(std::string(session));
    });
}

/* Plumb the instrument class (syminfo.type: "forex" / "stock" / "crypto" /
 * "futures" / ...) into syminfo_. Defaults to "crypto"; NULL/empty ignored. */
PF_API void strategy_set_syminfo_type(pf_strategy_t s, const char* type) {
    pf_cabi_void([&] {
        if (!s || !type) return;
        static_cast<pineforge::BacktestEngine*>(s)->set_syminfo_type(std::string(type));
    });
}

/* Generic string-member injection (ticker / tickerid / currency /
 * basecurrency / description / volumetype / type). Returns 0 when set, -1
 * for a NULL handle, unknown key or empty value. */
PF_API int strategy_set_syminfo_string(pf_strategy_t s, const char* key,
                                       const char* value) {
    return pf_cabi_int([&] {
        if (!s || !key || !value) return -1;
        return static_cast<pineforge::BacktestEngine*>(s)->set_syminfo_string(
                   std::string(key), std::string(value)) ? 0 : -1;
    });
}

/* Inject the instrument tick size (syminfo.mintick). Drives the directional
 * stop-entry snap (long ceil / short floor) and slippage = N*mintick economics.
 * Defaults to 0.01 (crypto/equity); set per-instrument (e.g. 0.25 for ES,
 * 0.00001 for FX). Non-positive values are ignored. */
PF_API void strategy_set_syminfo_mintick(pf_strategy_t s, double mintick) {
    pf_cabi_void([&] {
        if (!s) return;
        static_cast<pineforge::BacktestEngine*>(s)->set_syminfo_mintick(mintick);
    });
}

/* Inject the instrument point value (syminfo.pointvalue) — the $ per point per
 * contract multiplier applied to realized PnL and MFE/MAE. Defaults to 1.0
 * (crypto/equity); set per-instrument (e.g. 50 for ES). Non-positive ignored. */
PF_API void strategy_set_syminfo_pointvalue(pf_strategy_t s, double pointvalue) {
    pf_cabi_void([&] {
        if (!s) return;
        static_cast<pineforge::BacktestEngine*>(s)->set_syminfo_pointvalue(pointvalue);
    });
}

/* Inject a fundamental/exchange metadata value (shares_outstanding_total,
 * recommendations_*, target_price_*, …) by Pine member name. Without an
 * injection the corresponding syminfo.* read returns na. NULL key ignored. */
PF_API void strategy_set_syminfo_metadata(pf_strategy_t s, const char* key,
                                          double value) {
    pf_cabi_void([&] {
        if (!s || !key) return;
        static_cast<pineforge::BacktestEngine*>(s)->set_syminfo_metadata(
            std::string(key), value);
    });
}

PF_API int strategy_set_account_currency_fx_series(
        pf_strategy_t s, const int64_t* effective_from_ms,
        const double* account_per_quote, int n) {
    return pf_cabi_int([&] {
        if (!s) return -1;
        return static_cast<pineforge::BacktestEngine*>(s)
                       ->set_account_currency_fx_series(
                           effective_from_ms, account_per_quote, n)
            ? 0 : -1;
    });
}

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
PF_API int strategy_set_aux_security_feed(pf_strategy_t s,
                                          const pf_bar_t* bars,
                                          int n,
                                          const char* input_tf) {
    return pf_cabi_int([&] {
        if (!s || n < 0 || (n > 0 && (!bars || !input_tf))) return -1;
        const auto* native = reinterpret_cast<const pineforge::Bar*>(bars);
        return static_cast<pineforge::BacktestEngine*>(s)
                       ->set_aux_security_feed(
                           native, n,
                           input_tf ? std::string(input_tf) : std::string())
            ? 0 : -1;
    });
}
#endif

#ifdef PINEFORGE_HAS_NATIVE_SECURITY_FEED_V1
PF_API int strategy_set_native_security_feed(pf_strategy_t s,
                                             const char* timeframe,
                                             const pf_bar_t* bars,
                                             int n) {
    return pf_cabi_int([&] {
        if (!s || !timeframe || n < 0 || (n > 0 && !bars)) return -1;
        const auto* native = reinterpret_cast<const pineforge::Bar*>(bars);
        return static_cast<pineforge::BacktestEngine*>(s)
                       ->set_native_security_feed(std::string(timeframe), native, n)
            ? 0 : -1;
    });
}
#endif

/* See PF_ABI_VERSION doc in pineforge.h. */
PF_API int pf_abi_version(void) { return PF_ABI_VERSION; }

/* Return the runtime library version. */
PF_API pf_version_t pf_version_get(void) {
    pf_version_t v;
    v.major      = PINEFORGE_VERSION_MAJOR;
    v.minor      = PINEFORGE_VERSION_MINOR;
    v.patch      = PINEFORGE_VERSION_PATCH;
    v.commit_sha = PINEFORGE_GIT_SHA;
    return v;
}

PF_API const char* pf_version_string(void) {
    return PINEFORGE_VERSION_FULL;
}

static_assert(sizeof(pf_native_run_spec_v1) >= sizeof(uint32_t) + sizeof(const char*),
              "pf_native_run_spec_v1 must carry struct_size and string pointers");

PF_API int strategy_execution_contract(pf_strategy_t s) {
    try {
        if (!s) return -1;
        return static_cast<pineforge::BacktestEngine*>(s)->execution_contract();
    } catch (...) {
        return -1;
    }
}

PF_API int strategy_configure_native_v1(pf_strategy_t s, const pf_native_run_spec_v1* spec) {
    try {
        if (!s || !spec) return -1;
        auto* engine = static_cast<pineforge::BacktestEngine*>(s);
        if (!engine->native_bound()) return -1;
        if (spec->struct_size != sizeof(pf_native_run_spec_v1)) return -1;
        auto* host = dynamic_cast<pineforge::NativeStrategyHost*>(engine);
        if (!host) return -1;
        const auto require = [](const char* p) -> const char* {
            return p ? p : "";
        };
        if (!spec->session_key || !spec->input_tf || !spec->script_tf || !spec->ticker
            || !spec->tickerid || !spec->type || !spec->currency || !spec->basecurrency
            || !spec->description || !spec->volumetype || !spec->timezone
            || !spec->session || !spec->chart_timezone) {
            return -1;
        }
        if (spec->optional_mask & ~0xfu) return -1;
        pineforge::NativeRunSpec cpp;
        cpp.identity.session_key = require(spec->session_key);
        cpp.identity.run_number = spec->run_number;
        cpp.input_tf = require(spec->input_tf);
        cpp.script_tf = require(spec->script_tf);
        cpp.ticker = require(spec->ticker);
        cpp.tickerid = require(spec->tickerid);
        cpp.type = require(spec->type);
        cpp.currency = require(spec->currency);
        cpp.basecurrency = require(spec->basecurrency);
        cpp.description = require(spec->description);
        cpp.volumetype = require(spec->volumetype);
        cpp.timezone = require(spec->timezone);
        cpp.session = require(spec->session);
        cpp.chart_timezone = require(spec->chart_timezone);
        cpp.initial_capital = spec->initial_capital;
        cpp.point_value = spec->point_value;
        cpp.account_fx = spec->account_fx;
        cpp.price_tick = spec->price_tick;
        cpp.slippage_ticks = spec->slippage_ticks;
        cpp.fee_kind = static_cast<pineforge::NativeFeeKind>(spec->fee_kind);
        cpp.fee_value = spec->fee_value;
        cpp.close_execution = static_cast<pineforge::NativeCloseExecution>(spec->close_execution);
        cpp.allowed_open_directions =
            static_cast<pineforge::NativeOpenDirections>(spec->allowed_open_directions);
        if (spec->optional_mask & 1u) cpp.quantity_grid = spec->quantity_grid;
        if (spec->optional_mask & 2u) cpp.max_abs_units = spec->max_abs_units;
        if (spec->optional_mask & 4u) cpp.initial_margin_fraction = spec->initial_margin_fraction;
        if (spec->optional_mask & 8u) cpp.max_open_lots = spec->max_open_lots;
        const auto result = host->configure_native(cpp);
        return result.status == pineforge::NativeSetupStatus::Applied ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

} /* extern "C" */
