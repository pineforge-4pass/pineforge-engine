// A28 native-default witnesses.  These record the pre-transfer behavior that
// the source seams must preserve for a native-bound host.
#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

using namespace pineforge;
namespace x = pineforge::execution;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(value) do {                                                        \
    ++checks;                                                                    \
    if (!(value)) {                                                              \
        ++failures;                                                              \
        std::printf("FAIL %s:%d: %s\\n", __FILE__, __LINE__, #value);          \
    }                                                                            \
} while (0)

class NativeWitness final : public NativeStrategyHost {
public:
    int source_loss_days = 0;
    int source_loss_day = -1;
    double source_intraday_pnl = 0.0;
    int source_intraday_day = -1;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    x::PhysicalExecutionContext context() const {
        return {1700000000000LL, 7, {}, {}};
    }

    x::Result settle(const x::Action& action, const x::Fill& fill) {
        return settle_native_execution_at(action, fill, context());
    }

    x::Result settle_with_effects(const x::Action& action, const x::Fill& fill,
                                  const x::LifecycleEffects& effects) {
        return settle_with_context(action, fill, effects, context());
    }

    void poison_source_close_state() {
        source_loss_days = std::numeric_limits<int>::max();
        source_loss_day = 17;
        source_intraday_pnl = std::numeric_limits<double>::quiet_NaN();
        source_intraday_day = 29;
    }

    int loss_days() const { return source_loss_days; }
    int loss_day() const { return source_loss_day; }
    double intraday_pnl() const { return source_intraday_pnl; }
    int intraday_day() const { return source_intraday_day; }
    double metadata(const std::string& key) const { return get_syminfo_metadata(key); }
};

x::Fill fill(double price, const char* id, uint64_t incarnation) {
    return {price, id, "", incarnation, 0.0};
}

void check_native_metadata_and_aux_refusals() {
    NativeWitness host;
    bool metadata_refused = false;
    try {
        host.set_syminfo_metadata("qty_step", 0.25);
    } catch (const std::runtime_error& error) {
        metadata_refused = std::string(error.what())
            == "native host refuses source mutation: set_syminfo_metadata";
    }
    CHECK(metadata_refused);
    CHECK(std::isnan(host.metadata("qty_step")));

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    const Bar bars[] = {{100.0, 101.0, 99.0, 100.0, 1.0, 1700000000000LL}};
    bool aux_refused = false;
    try {
        (void)host.set_aux_security_feed(bars, 1, "1");
    } catch (const std::runtime_error& error) {
        aux_refused = std::string(error.what())
            == "native host refuses source mutation: set_aux_security_feed";
    }
    CHECK(aux_refused);
#else
#error "A28 witness requires the auxiliary-security feed surface"
#endif
}

void check_native_position_and_source_empty_settlement() {
    NativeWitness host;
    const auto opened = host.settle(order_action::Transact{2.5}, fill(100.0, "open", 1));
    CHECK(opened.status == x::Status::Applied);
    CHECK(host.physical_position().signed_units == 2.5);
    // S19's future base default must expose physical signed units, not the
    // source position-view freeze projection.
    CHECK(host.live_position_size() == 2.5);

    host.poison_source_close_state();
    const auto closed = host.settle(x::Flatten{}, fill(90.0, "close", 2));
    // Native settlement has no source close observation: the source-state
    // poison is inert, just as the future S20/S21 defaults are after their
    // local loss_day reset.
    CHECK(closed.status == x::Status::Applied);
    CHECK(host.loss_days() == std::numeric_limits<int>::max());
    CHECK(host.loss_day() == 17);
    CHECK(std::isnan(host.intraday_pnl()));
    CHECK(host.intraday_day() == 29);
}

void check_native_empty_lifecycle_and_rejection() {
    NativeWitness host;
    const auto opened = host.settle(order_action::Transact{1.0}, fill(100.0, "open", 1));
    CHECK(opened.status == x::Status::Applied);

    const x::LifecycleEffects empty{};
    const auto applied = host.settle_with_effects(x::Flatten{}, fill(90.0, "empty", 2), empty);
    // S22's source-empty lifecycle default accepts the empty value (nullopt);
    // S23/S24 consequently have no source work to apply.
    CHECK(applied.status == x::Status::Applied);

    NativeWitness invalid_host;
    const auto invalid_open = invalid_host.settle(order_action::Transact{1.0}, fill(100.0, "open", 1));
    CHECK(invalid_open.status == x::Status::Applied);
    x::LifecycleEffects nonempty;
    nonempty.removals.push_back({999, 999, {}, 0});
    const auto rejected = invalid_host.settle_with_effects(
        x::Flatten{}, fill(90.0, "invalid", 2), nonempty);
    CHECK(rejected.status == x::Status::InvalidLifecycle);
    CHECK(invalid_host.physical_position().signed_units == 1.0);
}

} // namespace

int main() {
    check_native_metadata_and_aux_refusals();
    check_native_position_and_source_empty_settlement();
    check_native_empty_lifecycle_and_rejection();
    std::printf("checks=%d failures=%d\\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
