#pragma once

#include <pineforge/bar.hpp>
#include <pineforge/magnifier.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace pineforge {

struct SymInfo;
struct StrategyOverrides;

inline namespace engine_script_run_v12 {

class BacktestEngine;

// Constructor-bound execution consumer. LegacyCompatibilityConsumer preserves
// the existing Pine call order. NativeExecutionConsumer owns native lifecycle,
// matching and the sole native timeline allocator. Neither is copyable.
class IExecutionConsumer {
public:
    IExecutionConsumer() = default;
    virtual ~IExecutionConsumer() = default;
    IExecutionConsumer(const IExecutionConsumer&) = delete;
    IExecutionConsumer& operator=(const IExecutionConsumer&) = delete;
    IExecutionConsumer(IExecutionConsumer&&) = delete;
    IExecutionConsumer& operator=(IExecutionConsumer&&) = delete;

    virtual bool is_native() const noexcept = 0;
    virtual void refuse_source_mutation(const char* operation) = 0;
    virtual uint64_t continuation_hash() const noexcept = 0;

    virtual void run_simple(BacktestEngine& engine, const Bar* bars, int n) = 0;
    virtual void run_tf(BacktestEngine& engine,
                        const Bar* input_bars, int n_input,
                        const std::string& input_tf,
                        const std::string& script_tf,
                        bool bar_magnifier,
                        int magnifier_samples,
                        MagnifierDistribution magnifier_dist) = 0;
    virtual void run_rich(BacktestEngine& engine,
                          const Bar* input_bars, int n_input,
                          const std::string& input_tf,
                          const std::string& script_tf,
                          const std::unordered_map<std::string, std::string>& inputs,
                          const SymInfo& syminfo,
                          const StrategyOverrides* overrides,
                          bool bar_magnifier,
                          int magnifier_samples,
                          MagnifierDistribution magnifier_dist) = 0;

    virtual bool stream_begin(BacktestEngine& engine,
                              const Bar* warmup_bars, int n_warmup,
                              const std::string& input_tf,
                              const std::string& script_tf) = 0;
    virtual bool stream_push_bar(BacktestEngine& engine, const Bar& bar) = 0;
    virtual bool stream_push_tick(BacktestEngine& engine, const TradeTick& tick) = 0;
    virtual bool stream_push_ticks(BacktestEngine& engine, const TradeTick* ticks, int n) = 0;
    virtual bool stream_advance_time(BacktestEngine& engine, int64_t timestamp_ms) = 0;
    virtual bool stream_end(BacktestEngine& engine, bool finalize_partial_input_bar) = 0;
};

std::unique_ptr<IExecutionConsumer> make_legacy_execution_consumer();
std::unique_ptr<IExecutionConsumer> make_native_execution_consumer();

}  // inline namespace engine_script_run_v12
}  // namespace pineforge
