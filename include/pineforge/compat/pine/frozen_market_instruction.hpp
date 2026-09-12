#pragma once

#include <pineforge/quantity_intent.hpp>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v13 { struct PendingOrder; }
}

namespace pineforge::compat::pine {

enum class FrozenMarketInstructionKind { Ordinary, Transaction, TargetedClose };

// Pine source interpretation retained at command admission. This is not a
// generic native reduce-position operation: its existing close-artifact path
// can open an artifact lot after the original side has disappeared. Native
// lowering of that behavior remains a separate adapter-boundary change.
// The transaction keeps its own admitted units and total transaction units;
// a targeted close uses QuantityRequest's original resolved Units amount.
class FrozenMarketInstruction {
public:
    struct Transaction {
        double own_units;
        double transaction_units;
    };
    struct TargetedClose {
        std::string target_id;
    };
    FrozenMarketInstruction() = default;
    static FrozenMarketInstruction transaction(double own, double total) {
        if (!std::isfinite(own) || own <= 0.0 || std::isnan(total) || total < own)
            throw std::invalid_argument("invalid frozen market transaction amounts");
        // Keep the existing execution-side finite-total guard. A positive
        // overflowed source sum is not silently replaced by an ordinary order
        // or a second default-sized amount by this representation change.
        return FrozenMarketInstruction(Transaction{own, total});
    }
    static FrozenMarketInstruction targeted_close(std::string target,
                                                  const QuantityRequest& request) {
        if (target.empty() || !request.intent()
            || request.intent()->kind() != QuantityIntent::Kind::Units
            || !std::isfinite(request.intent()->units()) || request.intent()->units() <= 0.0)
            throw std::invalid_argument("targeted close requires a resolved positive units request");
        return FrozenMarketInstruction(TargetedClose{std::move(target)});
    }
    FrozenMarketInstructionKind kind() const {
        return static_cast<FrozenMarketInstructionKind>(value_.index());
    }
    bool active() const { return !std::holds_alternative<std::monostate>(value_); }
    const Transaction* transaction() const { return std::get_if<Transaction>(&value_); }
    const TargetedClose* targeted_close() const { return std::get_if<TargetedClose>(&value_); }
    void revoke() { value_ = std::monostate{}; }
private:
    using Value = std::variant<std::monostate, Transaction, TargetedClose>;
    explicit FrozenMarketInstruction(Value value) : value_(std::move(value)) {}
    Value value_;
};

// Full-book admission is one source-policy step before native matching.
// source_scope_live is the engine's current selector result, not stored state.
void finalize_frozen_market_book(std::vector<PendingOrder>& orders, bool source_scope_live);

} // namespace pineforge::compat::pine

namespace pineforge {
using PineFrozenMarketInstruction = compat::pine::FrozenMarketInstruction;
} // namespace pineforge
