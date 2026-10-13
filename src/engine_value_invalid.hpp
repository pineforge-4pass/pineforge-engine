#pragma once

// Engine-derived invalid execution values (revision 3 section 4): the generic
// kernel-side throw for engine_command_value_invalid (engine_fault, not
// retryable). A value the engine derived from its own book, equity, margin,
// sizing, fee, slippage or settlement state is invalid when it is not finite
// or a checked operation overflowed; the stage names where. Header only, no
// durable state, no hash input. Engine-internal, not installed API.

#include <pineforge/run_failure.hpp>

#include <stdexcept>

namespace pineforge {

enum class EngineValueStage { sizing, margin, fee, slippage, relative_price, risk, settlement };
enum class EngineValueReason { nonfinite, invalid_unicode, numeric_overflow };

inline const char* engine_value_stage_name(EngineValueStage stage) noexcept {
    switch (stage) {
        case EngineValueStage::sizing: return "sizing";
        case EngineValueStage::margin: return "margin";
        case EngineValueStage::fee: return "fee";
        case EngineValueStage::slippage: return "slippage";
        case EngineValueStage::relative_price: return "relative_price";
        case EngineValueStage::risk: return "risk";
        case EngineValueStage::settlement: return "settlement";
    }
    return "sizing";
}

inline const char* engine_value_reason_name(EngineValueReason reason) noexcept {
    switch (reason) {
        case EngineValueReason::nonfinite: return "nonfinite";
        case EngineValueReason::invalid_unicode: return "invalid_unicode";
        case EngineValueReason::numeric_overflow: return "numeric_overflow";
    }
    return "nonfinite";
}

/// `field` is an engine-owned literal naming the derived value (for example
/// "sizing.quantity", "margin.required", "fee.commission",
/// "slippage.fill_price", "relative_price.level", "risk.threshold",
/// "settlement.row"); a lane that needs another name lists it in its report,
/// and the ownership manifest enumerates the set.
[[noreturn]] inline void throw_engine_command_value_invalid(
        const char* field, EngineValueStage stage, EngineValueReason reason) {
    throw coded<std::runtime_error>(
        RunFailureCode::engine_command_value_invalid,
        RunFailureArgs{{"field", field},
                       {"stage", engine_value_stage_name(stage)},
                       {"reason", engine_value_reason_name(reason)}},
        "the engine derived an invalid execution value");
}

}  // namespace pineforge
