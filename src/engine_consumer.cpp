#include "engine_internal.hpp"
#include "native_execution_consumer.hpp"
#include "cache_policy_validation.hpp"

#include <pineforge/execution_consumer.hpp>
#include <pineforge/native_host.hpp>

#include <stdexcept>
#include <limits>
#include <utility>
#include <unistd.h>

namespace pineforge {
CacheCapacity CacheDefaults::platform_command_pages() noexcept {
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || static_cast<unsigned long>(page_size) > 64 * 1024)
        return {};
    const auto page = static_cast<std::size_t>(page_size);
    return lines(page, 64 * 1024 / page);
}

inline namespace engine_script_run_v19 {

namespace {
WindowCacheCapacity numeric_window_capacity(CacheCapacity capacity) noexcept {
    if (capacity.capacity_bytes == std::numeric_limits<std::size_t>::max())
        return {};
    return {capacity.lines(1024 * sizeof(double))};
}
}

BacktestEngine::BacktestEngine(NativeConsumerBindTag) {
    execution_consumer_slot_.native = true;
    execution_consumer_slot_.ptr = make_native_execution_consumer();
}

BacktestEngine::~BacktestEngine() = default;

IExecutionConsumer& BacktestEngine::execution_consumer() {
    if (!execution_consumer_slot_.ptr) {
        execution_consumer_slot_.ptr = make_native_execution_consumer();
    }
    return *execution_consumer_slot_.ptr;
}

const IExecutionConsumer& BacktestEngine::execution_consumer() const {
    if (!execution_consumer_slot_.ptr) {
        execution_consumer_slot_.ptr = make_native_execution_consumer();
    }
    return *execution_consumer_slot_.ptr;
}

bool BacktestEngine::native_bound() const {
    return true;
}

int BacktestEngine::execution_contract() const {
    return 2;
}

void BacktestEngine::guard_native_mutation(const char* operation) {
    // ab9714be:src/engine_consumer.cpp LegacyCompatibilityConsumer::refuse
    // was a no-op on the source-route handle. L8h made this guard inert only
    // while stream_warmup_mode_ is set (cleared on the first realtime tick).
    // Source hosts also set host_mutation_guard_inert_ for the handle
    // lifetime so a C-ABI FX setter after the first realtime tick still
    // returns false without latching UnsupportedSource. Native hosts never
    // set either flag, so their in-run setter still throws (P1-22).
    if (stream_warmup_mode_ || host_mutation_guard_inert_) return;
    execution_consumer().refuse_source_mutation(operation);
}

void BacktestEngine::set_syminfo_metadata(const std::string& key, double value) {
    guard_native_mutation("set_syminfo_metadata");
    syminfo_metadata_[key] = value;
    if (key == "qty_step") {
        qty_step_ = (std::isfinite(value) && value > 0.0) ? value : 0.0;
        syminfo_.qty_step = qty_step_;
    }
    if (key == "account_currency_fx") {
        account_currency_fx_ =
            (std::isfinite(value) && value > 0.0) ? value : 1.0;
    }
}

void BacktestEngine::set_cache_policy(const CacheDefaults& policy) {
    if (report_sink_setter_status() != 0)
        detail::reject_cache_policy("frozen", "cache policy is frozen for the current run");
    (void)CacheDefaults::ledger_cache_rows(policy.ledger_rows);
    as_native_consumer(execution_consumer()).set_cache_policy_config(policy);
}

int BacktestEngine::set_cache_policy_v1(const pf_cache_policy_v1* descriptor) {
    if (report_sink_setter_status() != 0)
        detail::reject_cache_policy("frozen", "cache policy is frozen for the current run");
    static constexpr std::array<detail::CachePolicyField, 19> fields{{
        {PF_CACHE_KIND_LEDGER_CACHE, &CacheDefaults::ledger_rows},
        {PF_CACHE_KIND_TRADE_PAGES, &CacheDefaults::trade_pages},
        {PF_CACHE_KIND_DEPENDENCY_PAGES, &CacheDefaults::dependency_pages},
        {PF_CACHE_KIND_EXIT_PAGES, &CacheDefaults::exit_pages},
        {PF_CACHE_KIND_BOOK_PAGES, &CacheDefaults::book_pages},
        {PF_CACHE_KIND_RETAINED_SEQUENCE_PAGES, &CacheDefaults::retained_sequence_pages},
        {PF_CACHE_KIND_COHORT_ID_PAGES, &CacheDefaults::cohort_id_pages},
        {PF_CACHE_KIND_EXIT_RANK_PAGES, &CacheDefaults::exit_rank_pages},
        {PF_CACHE_KIND_ERASED_MINIMA_PAGES, &CacheDefaults::erased_minima_pages},
        {PF_CACHE_KIND_CONSUMED_CYCLE_PAGES, &CacheDefaults::consumed_cycle_pages},
        {PF_CACHE_KIND_POSITION_TAIL_PAGES, &CacheDefaults::position_tail_pages},
        {PF_CACHE_KIND_SUCCESSOR_ROOT_PAGES, &CacheDefaults::successor_root_pages},
        {PF_CACHE_KIND_RECEIPT_TAIL_PAGES, &CacheDefaults::receipt_tail_pages},
        {PF_CACHE_KIND_CHAIN_SEGMENT_PAGES, &CacheDefaults::chain_segment_pages},
        {PF_CACHE_KIND_EQUITY_CURVE_PAGES, &CacheDefaults::equity_curve_pages},
        {PF_CACHE_KIND_TA_HISTORY_PAGES, &CacheDefaults::ta_history_pages},
        {PF_CACHE_KIND_FEED_CALENDAR_PAGES, &CacheDefaults::feed_calendar_pages},
        {PF_CACHE_KIND_RECORDED_SERIES_PAGES, &CacheDefaults::recorded_series_pages},
        {PF_CACHE_KIND_PROJECTION_EXIT_PAGES, &CacheDefaults::projection_exit_pages},
    }};
    const CacheDefaults next = detail::copy_cache_policy_v1(descriptor, fields);
    as_native_consumer(execution_consumer()).set_cache_policy_config(next);
    return 0;
}

const CacheDefaults& BacktestEngine::cache_policy_config() const {
    return as_native_consumer(execution_consumer()).cache_policy_config();
}

const CacheDefaults& BacktestEngine::cache_policy_for_run() const {
    return as_native_consumer(execution_consumer()).cache_policy_for_run();
}

void BacktestEngine::run(const Bar* bars, int n) {
    const WindowCacheScope cache_scope(numeric_window_capacity(cache_policy_config().ta_history_pages));
    execution_consumer().run_simple(*this, bars, n);
}

void BacktestEngine::run(const Bar* input_bars, int n_input,
                         const std::string& input_tf,
                         const std::string& script_tf,
                         bool bar_magnifier,
                         int magnifier_samples,
                         MagnifierDistribution magnifier_dist) {
    const WindowCacheScope cache_scope(numeric_window_capacity(cache_policy_config().ta_history_pages));
    execution_consumer().run_tf(*this, input_bars, n_input, input_tf, script_tf,
                                bar_magnifier, magnifier_samples, magnifier_dist);
}

void BacktestEngine::run(const Bar* input_bars, int n_input,
                         const std::string& input_tf,
                         const std::string& script_tf,
                         const std::unordered_map<std::string, std::string>& inputs,
                         const SymInfo& syminfo,
                         const void* overrides,
                         bool bar_magnifier,
                         int magnifier_samples,
                         MagnifierDistribution magnifier_dist) {
    const WindowCacheScope cache_scope(numeric_window_capacity(cache_policy_config().ta_history_pages));
    execution_consumer().run_rich(*this, input_bars, n_input, input_tf, script_tf,
                                  inputs, syminfo, overrides, bar_magnifier,
                                  magnifier_samples, magnifier_dist);
}

bool BacktestEngine::stream_begin(const Bar* warmup_bars, int n_warmup,
                                  const std::string& input_tf,
                                  const std::string& script_tf) {
    const WindowCacheScope cache_scope(numeric_window_capacity(cache_policy_config().ta_history_pages));
    return execution_consumer().stream_begin(*this, warmup_bars, n_warmup,
                                             input_tf, script_tf);
}

bool BacktestEngine::stream_push_bar(const Bar& bar) {
    const WindowCacheScope cache_scope(numeric_window_capacity(cache_policy_for_run().ta_history_pages));
    return execution_consumer().stream_push_bar(*this, bar);
}

bool BacktestEngine::stream_push_tick(const TradeTick& tick) {
    const WindowCacheScope cache_scope(numeric_window_capacity(cache_policy_for_run().ta_history_pages));
    return execution_consumer().stream_push_tick(*this, tick);
}

bool BacktestEngine::stream_push_ticks(const TradeTick* ticks, int n) {
    const WindowCacheScope cache_scope(numeric_window_capacity(cache_policy_for_run().ta_history_pages));
    return execution_consumer().stream_push_ticks(*this, ticks, n);
}

bool BacktestEngine::stream_advance_time(int64_t timestamp_ms) {
    const WindowCacheScope cache_scope(numeric_window_capacity(cache_policy_for_run().ta_history_pages));
    return execution_consumer().stream_advance_time(*this, timestamp_ms);
}

bool BacktestEngine::stream_end(bool finalize_partial_input_bar) {
    const WindowCacheScope cache_scope(numeric_window_capacity(cache_policy_for_run().ta_history_pages));
    return execution_consumer().stream_end(*this, finalize_partial_input_bar);
}

void BacktestEngine::source_stream_entry_comment(const PyramidEntry&, std::string&) const {}

int BacktestEngine::observe_last_bar_dual_entry_path_v1() const { return 0; }
int BacktestEngine::observe_pending_count_v1() const { return 0; }
int BacktestEngine::observe_pending_copy_v1(int, pf_pending_order_v1_t*) const { return -1; }
int BacktestEngine::observe_probe_fill_qty(int, double, double*, int*, int*) const {
    return -1;
}
int BacktestEngine::observe_pending_level_resolved(int) const { return -1; }
int BacktestEngine::observe_pending_effective_levels(int, double*, double*, double*) const {
    return -1;
}
double BacktestEngine::observe_trail_best_price_v1() const {
    return trail_best_price_;
}

// Host run-mode overrides behind the frozen C setters (engine.hpp): the
// kernel models no forming tail bar. Accepted and inert, exactly what the
// flags always were on a native host; false tells a C++ caller that no
// such mode exists here.
bool BacktestEngine::set_realtime_tail(bool, int) {
    guard_native_mutation("set_realtime_tail");
    return false;
}
bool BacktestEngine::set_probe_suppress_tail_logic(bool) {
    guard_native_mutation("set_probe_suppress_tail_logic");
    return false;
}

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
// This setter is the source host's split-feed door: its contract is that
// host's chart-slice mapping, and the kernel base keeps answering false
// without a word, as every pre-begin setter pin expects. A host with no
// source layer declares its finer bars in the run spec instead
// (NativeRunSpec::auxiliary_feed, or NativeStrategyHost::declare_auxiliary_feed
// at begin).
bool BacktestEngine::set_aux_security_feed(const Bar*, int, const std::string&) {
    guard_native_mutation("set_aux_security_feed");
    return false;
}
bool BacktestEngine::source_aux_security_feed_enabled() const { return false; }
void BacktestEngine::source_aux_security_input_view(const Bar*&, int&) const {}
#endif

// The same shape for the four request-data doors of a source host
// (strategy_set_symbol_feed and its siblings): the kernel base answers false
// without a word. A host with no source layer installs another instrument's
// bars as NativeRunSpec::instrument_feeds; recorded request values and symbol
// facts are a source language's, and no kernel decision reads them.
bool BacktestEngine::set_symbol_feed(const std::string&, const std::string&, const Bar*,
                                     const int64_t*, int) {
    guard_native_mutation("set_symbol_feed");
    return false;
}
bool BacktestEngine::set_symbol_feed_column(const std::string&, const std::string&,
                                            const std::string&, const double*, int) {
    guard_native_mutation("set_symbol_feed_column");
    return false;
}
bool BacktestEngine::set_symbol_facts(const std::string&, const std::string&,
                                      const std::string&) {
    guard_native_mutation("set_symbol_facts");
    return false;
}
bool BacktestEngine::set_recorded_series(const std::string&, const int64_t*, const double*,
                                         int) {
    guard_native_mutation("set_recorded_series");
    return false;
}

double BacktestEngine::initial_capital() const {
    // Configure stages the accepted spec; begin copies its capital into
    // initial_capital_, which report metrics use. Read the spec even before
    // begin, including after a completed host is configured for another run.
    if (const auto* host = dynamic_cast<const NativeStrategyHost*>(this)) {
        if (const auto* spec = host->native_state().spec) {
            return spec->initial_capital;
        }
    }
    return initial_capital_;
}

}  // inline namespace engine_script_run_v19
}  // namespace pineforge
