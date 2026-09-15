// A35 pure-native witness for read-only trail-state observation.
// No generated source host or adapter participates.
#include "native_current_fixture.hpp"

#include <array>
#include <optional>
#include <type_traits>

using namespace r4_test;

static_assert(std::is_trivially_copyable_v<NativeTrailState>);

namespace {

class TrailStateHost final : public Host {
public:
    no::RequestHandle trail;
    std::optional<NativeTrailState> at_open;
    std::optional<NativeTrailState> after_retrace;
    std::uint64_t open_hash_before = 0;
    std::uint64_t open_hash_after = 0;
    std::uint64_t retrace_hash_before = 0;
    std::uint64_t retrace_hash_after = 0;

    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        open_hash_before = native_continuation_hash();
        at_open = trail_state(trail);
        open_hash_after = native_continuation_hash();
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        retrace_hash_before = native_continuation_hash();
        after_retrace = trail_state(trail);
        retrace_hash_after = native_continuation_hash();
        Host::on_native_bar(bar, context);
    }
};

void observes_activation_best_level_and_ordinal() {
    TrailStateHost host;
    host.beginning = [&](Host& base) {
        const auto opening = put(base, tx(1.0, "trail-opening"));
        no::Request trail{no::Reduce{no::OwnerOpenedUnits{}}, "observed-trail", ""};
        trail.owner = no::WaitForApplied{opening};
        trail.trigger = no::Trail{20.0, 102.0};
        host.trail = put(base, trail);
    };

    REQUIRE(host.configure_native(spec("l5k-trail-state")).status
            == NativeSetupStatus::Applied);
    // Low-first path: the trail arms at 102, improves through 110, and then
    // retraces to 105 without touching its current level 90.
    const Bar bar{100.0, 110.0, 99.0, 105.0, 1.0, T};
    host.run(&bar, 1);

    REQUIRE(host.at_open.has_value());
    CHECK(!host.at_open->activated);
    CHECK(host.at_open->best_price == 0.0);
    CHECK(host.at_open->current_level == 0.0);
    CHECK(host.at_open->activation_ordinal == 0);
    CHECK(host.open_hash_before == host.open_hash_after);

    REQUIRE(host.after_retrace.has_value());
    CHECK(host.after_retrace->activated);
    near(host.after_retrace->best_price, 110.0);
    near(host.after_retrace->current_level, 90.0);
    CHECK(host.after_retrace->activation_ordinal != 0);
    CHECK(host.retrace_hash_before == host.retrace_hash_after);

    const auto activations = events<no::ActivatedEvent>(host);
    const auto arm = std::find_if(activations.begin(), activations.end(),
        [&](const no::ActivatedEvent& event) {
            return event.definition && event.definition->handle == host.trail
                && event.kind == no::ActivationKind::TrailArm;
        });
    REQUIRE(arm != activations.end());
    CHECK(host.after_retrace->activation_ordinal == arm->ordinal);
    CHECK(arm->cursor.point.path_phase == NativePathPhase::High);
    CHECK(host.physical_position().signed_units == 1.0);
    completed(host);
}

}  // namespace

int main() {
    test("trail state at open and after retrace",
         observes_activation_best_level_and_ordinal);
    std::printf("L5k native trail state: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
