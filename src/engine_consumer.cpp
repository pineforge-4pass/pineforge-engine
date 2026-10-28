#include "engine_internal.hpp"
#include "native_execution_consumer.hpp"

#include <pineforge/detail/matrix_store.hpp>
#include <pineforge/execution_consumer.hpp>
#include <pineforge/native_host.hpp>

#include <stdexcept>
#include <limits>
#include <utility>

namespace pineforge {
inline namespace engine_script_run_v19 {

namespace {

// Every public call below that runs script code makes the engine's run the
// calling thread's matrix Runtime source (detail/matrix_store.hpp), so the
// matrices a script creates live in the run's own Store, and the call's matrix
// work runs in one transaction on it; outside these calls a library caller
// keeps native matrices. The run opens its Runtime with its first matrix.
// When the outermost call returns with no stream live (a batch
// run's return, stream_end, a refused or failed stream_begin) the run is over
// and the engine drops its share: the Runtime closes, and its scratch bytes
// are freed, with the last matrix, snapshot or borrow still reading it (a
// generated strategy resets its matrices in the next run's
// prepare_script_run, or frees them with the strategy), and the next run
// opens its own. A stream keeps its Runtime from stream_begin to stream_end.
class MatrixRunCall {
public:
    explicit MatrixRunCall(BacktestEngine& engine)
        : engine_(engine),
          run_(NativeExecutionConsumer::bound(engine).matrix_run()),
          scope_(run_) {}
    MatrixRunCall(const MatrixRunCall&) = delete;
    MatrixRunCall& operator=(const MatrixRunCall&) = delete;
    ~MatrixRunCall() {
        if (scope_.outermost() && !engine_.stream_is_realtime()) run_.close();
    }

private:
    const BacktestEngine& engine_;
    detail::matrix_store::RunRuntime& run_;
    detail::matrix_store::RunScope scope_;
};

}  // namespace

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

void BacktestEngine::run(const Bar* bars, int n) {
    const MatrixRunCall matrices(*this);
    execution_consumer().run_simple(*this, bars, n);
}

void BacktestEngine::run(const Bar* input_bars, int n_input,
                         const std::string& input_tf,
                         const std::string& script_tf,
                         bool bar_magnifier,
                         int magnifier_samples,
                         MagnifierDistribution magnifier_dist) {
    const MatrixRunCall matrices(*this);
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
    const MatrixRunCall matrices(*this);
    execution_consumer().run_rich(*this, input_bars, n_input, input_tf, script_tf,
                                  inputs, syminfo, overrides, bar_magnifier,
                                  magnifier_samples, magnifier_dist);
}

bool BacktestEngine::stream_begin(const Bar* warmup_bars, int n_warmup,
                                  const std::string& input_tf,
                                  const std::string& script_tf) {
    const MatrixRunCall matrices(*this);
    return execution_consumer().stream_begin(*this, warmup_bars, n_warmup,
                                             input_tf, script_tf);
}

bool BacktestEngine::stream_push_bar(const Bar& bar) {
    const MatrixRunCall matrices(*this);
    return execution_consumer().stream_push_bar(*this, bar);
}

bool BacktestEngine::stream_push_tick(const TradeTick& tick) {
    const MatrixRunCall matrices(*this);
    return execution_consumer().stream_push_tick(*this, tick);
}

bool BacktestEngine::stream_push_ticks(const TradeTick* ticks, int n) {
    const MatrixRunCall matrices(*this);
    return execution_consumer().stream_push_ticks(*this, ticks, n);
}

bool BacktestEngine::stream_advance_time(int64_t timestamp_ms) {
    const MatrixRunCall matrices(*this);
    return execution_consumer().stream_advance_time(*this, timestamp_ms);
}

bool BacktestEngine::stream_end(bool finalize_partial_input_bar) {
    const MatrixRunCall matrices(*this);
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
