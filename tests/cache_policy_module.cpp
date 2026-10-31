#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/series.hpp>
#include "cache_policy_module_fixture.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace {
class ModuleProbe final : public pineforge::source::PineStrategyHost {
public:
    std::unique_ptr<pineforge::DynamicRingBuffer<double>> values;
    void prepare_script_run(const pineforge::Bar*, int, bool) override {
        // This constructor is in the loaded module, while the run scope and
        // setter are in its linked runtime. An executable-only TLS test
        // cannot prove that this constructor receives the handle's policy.
        values = std::make_unique<pineforge::DynamicRingBuffer<double>>(kCachePolicyModuleValues);
    }
    void on_source_bar(const pineforge::Bar& bar) override {
        values->push_front(bar.close);
    }
};
}

extern "C" PF_API pf_strategy_t cache_policy_module_create() {
    try { return static_cast<pf_strategy_t>(new ModuleProbe); }
    catch (...) { return nullptr; }
}

extern "C" PF_API void cache_policy_module_free(pf_strategy_t strategy) {
    delete static_cast<ModuleProbe*>(strategy);
}

extern "C" PF_API int cache_policy_module_capture(
        pf_strategy_t strategy, const pf_bar_t* input, int count,
        double* output, std::size_t output_count, CachePolicyModuleObservation* observation) {
    if (!strategy || !input || count <= 0 || !output
        || output_count != kCachePolicyModuleValues || !observation) return -1;
    try {
        auto& engine = *static_cast<ModuleProbe*>(strategy);
        std::vector<pineforge::Bar> bars;
        bars.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            const auto& row = input[i];
            bars.push_back({row.open, row.high, row.low, row.close, row.volume, row.timestamp});
        }
        engine.run(bars.data(), count);
        // Also forces the runtime C ABI object into the module's static link.
        const char* error = strategy_get_last_error(strategy);
        if (engine.last_run_status() != 0 || (error && *error) || !engine.values
            || engine.values->size() != output_count) return -1;
        for (std::size_t i = 0; i < output_count; ++i) output[i] = (*engine.values)[i];
        const auto stats = engine.values->cache_stats();
        const auto shared = engine.values->read_cache_stats();
        observation->pages = engine.values->cache_capacity().pages;
        observation->resident_payload_bytes = stats.resident_payload_bytes;
        observation->resident_metadata_bytes = stats.resident_metadata_bytes;
        observation->read_cache_payload_bytes = shared.current_payload_bytes;
        observation->read_cache_metadata_bytes = shared.current_metadata_bytes;
        observation->backing_reads = stats.backing_reads;
        observation->backing_read_bytes = stats.backing_read_bytes;
        return 0;
    } catch (...) {
        return -1;
    }
}
