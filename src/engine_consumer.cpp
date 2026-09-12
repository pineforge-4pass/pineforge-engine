#include "engine_internal.hpp"

#include <pineforge/execution_consumer.hpp>
#include <pineforge/native_host.hpp>

#include <stdexcept>
#include <utility>

namespace pineforge {
inline namespace engine_script_run_v12 {

class LegacyCompatibilityConsumer final : public IExecutionConsumer {
public:
    bool is_native() const noexcept override { return false; }
    void refuse_source_mutation(const char*) override {}
    uint64_t continuation_hash() const noexcept override { return 0; }

    void run_simple(BacktestEngine& engine, const Bar* bars, int n) override {
        engine.legacy_run_simple(bars, n);
    }
    void run_tf(BacktestEngine& engine,
                const Bar* input_bars, int n_input,
                const std::string& input_tf,
                const std::string& script_tf,
                bool bar_magnifier,
                int magnifier_samples,
                MagnifierDistribution magnifier_dist) override {
        engine.legacy_run_tf(input_bars, n_input, input_tf, script_tf,
                             bar_magnifier, magnifier_samples, magnifier_dist);
    }
    void run_rich(BacktestEngine& engine,
                  const Bar* input_bars, int n_input,
                  const std::string& input_tf,
                  const std::string& script_tf,
                  const std::unordered_map<std::string, std::string>& inputs,
                  const SymInfo& syminfo,
                  const StrategyOverrides* overrides,
                  bool bar_magnifier,
                  int magnifier_samples,
                  MagnifierDistribution magnifier_dist) override {
        engine.legacy_run_rich(input_bars, n_input, input_tf, script_tf, inputs,
                               syminfo, overrides, bar_magnifier, magnifier_samples,
                               magnifier_dist);
    }
    bool stream_begin(BacktestEngine& engine,
                      const Bar* warmup_bars, int n_warmup,
                      const std::string& input_tf,
                      const std::string& script_tf) override {
        return engine.legacy_stream_begin(warmup_bars, n_warmup, input_tf, script_tf);
    }
    bool stream_push_bar(BacktestEngine& engine, const Bar& bar) override {
        return engine.legacy_stream_push_bar(bar);
    }
    bool stream_push_tick(BacktestEngine& engine, const TradeTick& tick) override {
        return engine.legacy_stream_push_tick(tick);
    }
    bool stream_push_ticks(BacktestEngine& engine, const TradeTick* ticks, int n) override {
        return engine.legacy_stream_push_ticks(ticks, n);
    }
    bool stream_advance_time(BacktestEngine& engine, int64_t timestamp_ms) override {
        return engine.legacy_stream_advance_time(timestamp_ms);
    }
    bool stream_end(BacktestEngine& engine, bool finalize_partial_input_bar) override {
        return engine.legacy_stream_end(finalize_partial_input_bar);
    }
};

std::unique_ptr<IExecutionConsumer> make_legacy_execution_consumer() {
    return std::make_unique<LegacyCompatibilityConsumer>();
}

BacktestEngine::BacktestEngine(compat::pine::CapAttachment cap_attachment)
    : max_intraday_filled_orders_(cap_attachment) {
    execution_consumer_slot_.native = false;
    execution_consumer_slot_.ptr = make_legacy_execution_consumer();
}

BacktestEngine::BacktestEngine(NativeConsumerBindTag,
                               compat::pine::CapAttachment cap_attachment)
    : max_intraday_filled_orders_(cap_attachment) {
    execution_consumer_slot_.native = true;
    execution_consumer_slot_.ptr = make_native_execution_consumer();
}

BacktestEngine::~BacktestEngine() = default;

IExecutionConsumer& BacktestEngine::execution_consumer() {
    if (!execution_consumer_slot_.ptr) {
        execution_consumer_slot_.ptr = execution_consumer_slot_.native
            ? make_native_execution_consumer()
            : make_legacy_execution_consumer();
    }
    return *execution_consumer_slot_.ptr;
}

const IExecutionConsumer& BacktestEngine::execution_consumer() const {
    if (!execution_consumer_slot_.ptr) {
        execution_consumer_slot_.ptr = execution_consumer_slot_.native
            ? make_native_execution_consumer()
            : make_legacy_execution_consumer();
    }
    return *execution_consumer_slot_.ptr;
}

bool BacktestEngine::native_bound() const {
    return execution_consumer().is_native();
}

int BacktestEngine::execution_contract() const {
    return native_bound() ? 2 : 1;
}

void BacktestEngine::guard_native_mutation(const char* operation) {
    execution_consumer().refuse_source_mutation(operation);
}

void BacktestEngine::run(const Bar* bars, int n) {
    execution_consumer().run_simple(*this, bars, n);
}

void BacktestEngine::run(const Bar* input_bars, int n_input,
                         const std::string& input_tf,
                         const std::string& script_tf,
                         bool bar_magnifier,
                         int magnifier_samples,
                         MagnifierDistribution magnifier_dist) {
    execution_consumer().run_tf(*this, input_bars, n_input, input_tf, script_tf,
                                bar_magnifier, magnifier_samples, magnifier_dist);
}

void BacktestEngine::run(const Bar* input_bars, int n_input,
                         const std::string& input_tf,
                         const std::string& script_tf,
                         const std::unordered_map<std::string, std::string>& inputs,
                         const SymInfo& syminfo,
                         const StrategyOverrides* overrides,
                         bool bar_magnifier,
                         int magnifier_samples,
                         MagnifierDistribution magnifier_dist) {
    execution_consumer().run_rich(*this, input_bars, n_input, input_tf, script_tf,
                                  inputs, syminfo, overrides, bar_magnifier,
                                  magnifier_samples, magnifier_dist);
}

bool BacktestEngine::stream_begin(const Bar* warmup_bars, int n_warmup,
                                  const std::string& input_tf,
                                  const std::string& script_tf) {
    return execution_consumer().stream_begin(*this, warmup_bars, n_warmup,
                                             input_tf, script_tf);
}

bool BacktestEngine::stream_push_bar(const Bar& bar) {
    return execution_consumer().stream_push_bar(*this, bar);
}

bool BacktestEngine::stream_push_tick(const TradeTick& tick) {
    return execution_consumer().stream_push_tick(*this, tick);
}

bool BacktestEngine::stream_push_ticks(const TradeTick* ticks, int n) {
    return execution_consumer().stream_push_ticks(*this, ticks, n);
}

bool BacktestEngine::stream_advance_time(int64_t timestamp_ms) {
    return execution_consumer().stream_advance_time(*this, timestamp_ms);
}

bool BacktestEngine::stream_end(bool finalize_partial_input_bar) {
    return execution_consumer().stream_end(*this, finalize_partial_input_bar);
}

}  // inline namespace engine_script_run_v12
}  // namespace pineforge
