// A39 P0-10/P1-16/P1-18: the switched adapter must use the complete
// ab9714be ShortSeed qualification and must never project a role from a plan
// that has not passed that qualification at the broker-open boundary.
#include <pineforge/compat/pine/market_admission.hpp>
#include <pineforge/source/pine_native_host.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                        \
    ++checks;                                                                   \
    if (!(expr)) {                                                              \
        ++failures;                                                             \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr);           \
    }                                                                           \
} while (false)

template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};

struct QualifyTag {
    using type = bool (source::PineExecutionAdapter::*)(
        const source::ShortSeedPlan&) const;
    friend type access(QualifyTag);
};
template struct PrivateAccess<QualifyTag,
    &source::PineExecutionAdapter::qualify_short_seed_plan>;

struct PendingPlanTag {
    using type = source::PendingShortSeedPlan source::PineExecutionAdapter::*;
    friend type access(PendingPlanTag);
};
template struct PrivateAccess<PendingPlanTag,
    &source::PineExecutionAdapter::pending_short_seed_>;

struct PlacementTag {
    using type = source::PlacementTable source::PineExecutionAdapter::*;
    friend type access(PlacementTag);
};
template struct PrivateAccess<PlacementTag,
    &source::PineExecutionAdapter::placement_>;

struct LiveHandlesTag {
    using type = std::vector<no::RequestHandle> source::PineExecutionAdapter::*;
    friend type access(LiveHandlesTag);
};
template struct PrivateAccess<LiveHandlesTag,
    &source::PineExecutionAdapter::live_handles_>;

// The adapter's single live-membership writer: it keeps the per-family live
// counters in step with the live handle list.
struct RememberLiveTag {
    using type = void (source::PineExecutionAdapter::*)(
        const no::RequestHandle&, source::PlacementSnapshot&&);
    friend type access(RememberLiveTag);
};
template struct PrivateAccess<RememberLiveTag,
    &source::PineExecutionAdapter::remember_live>;

struct LiveCountMismatchTag {
    using type = int (source::PineExecutionAdapter::*)() const noexcept;
    friend type access(LiveCountMismatchTag);
};
template struct PrivateAccess<LiveCountMismatchTag,
    &source::PineExecutionAdapter::live_count_mismatch>;

struct ActivatePlanTag {
    using type = void (source::PineExecutionAdapter::*)(const NativeDecisionContext&);
    friend type access(ActivatePlanTag);
};
template struct PrivateAccess<ActivatePlanTag,
    &source::PineExecutionAdapter::activate_short_seed_plan_at_open>;

struct BrokerEpochTag {
    using type = std::uint64_t source::PineExecutionAdapter::*;
    friend type access(BrokerEpochTag);
};
template struct PrivateAccess<BrokerEpochTag,
    &source::PineExecutionAdapter::broker_open_epoch_>;

struct StreamModeTag {
    using type = bool source::PineExecutionAdapter::*;
    friend type access(StreamModeTag);
};
template struct PrivateAccess<StreamModeTag,
    &source::PineExecutionAdapter::stream_mode_>;

struct BarMagnifierTag {
    using type = bool source::PineExecutionAdapter::*;
    friend type access(BarMagnifierTag);
};
template struct PrivateAccess<BarMagnifierTag,
    &source::PineExecutionAdapter::bar_magnifier_>;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Bar bar(std::int64_t timestamp) {
    return {100.0, 100.0, 100.0, 100.0, 1.0, timestamp};
}

enum class QualificationCase {
    Baseline,
    Magnifier,
    NonHundredMargins,
    StreamPhase,
    RejectedCommand,
    IncarnationGap,
    FillBorn,
    NamedCancelRecreation,
    ReservationCapture,
    MaterializeCarryMismatch,
};

class QualificationProbe final : public source::PineNativeHost {
public:
    explicit QualificationProbe(QualificationCase which, bool exercise_openings = false)
        : which_(which), exercise_openings_(exercise_openings) {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        config.slippage = 0;
        config.commission_value = 0.0;
        if (which_ == QualificationCase::NonHundredMargins) {
            config.margin_long = 50.0;
            config.margin_short = 50.0;
        }
        configure_pine_strategy(config);
    }

    bool baseline = false;
    bool qualified = false;
    bool roles_before_qualification_zero = false;
    int openings_checked = 0;
    int activations_checked = 0;

    void on_source_bar(const Bar&) override {
        // No fill recalculations: each source callback follows one real host
        // opening. CHECK stays active when the library is built with NDEBUG.
        CHECK((adapter_.*access(LiveCountMismatchTag{}))() == -1);
        ++openings_checked;
        if (pine_bar_index() == 0) {
            strategy_entry("seed-id", false);
            return;
        }
        if (pine_bar_index() != 1) return;

        strategy_entry("long-leg", true);
        strategy_entry("seed-id", false);
        strategy_close("long-leg");
        strategy_close("seed-id");

        // The production scheduler performs this flush immediately after the
        // callback.  Do it here so the test can evaluate the complete pending
        // three-object book while the current native decision point is still
        // present.  Advancing only the adapter's broker epoch models the next
        // open at which ab9714be ran the qualification.
        adapter_.flush_pending_entries();
        auto& pending = adapter_.*access(PendingPlanTag{});
        auto& epoch = adapter_.*access(BrokerEpochTag{});
        CHECK(pending.ready);
        roles_before_qualification_zero =
            adapter_.short_seed_collision_role_v1(pending.plan.long_entry) == 0
            && adapter_.short_seed_collision_role_v1(pending.plan.materialize_long) == 0
            && adapter_.short_seed_collision_role_v1(pending.plan.final_short) == 0;
        ++epoch;
        baseline = (adapter_.*access(QualifyTag{}))(pending.plan);

        auto& placements = adapter_.*access(PlacementTag{});
        switch (which_) {
        case QualificationCase::Magnifier:
            adapter_.*access(BarMagnifierTag{}) = true;
            break;
        case QualificationCase::StreamPhase:
            adapter_.*access(StreamModeTag{}) = true;
            break;
        case QualificationCase::RejectedCommand: {
            const auto id = adapter_.admission_journal.next_sequence();
            auto observation = std::make_shared<admission::CommandObservation>();
            observation->command = id;
            observation->bar = placements.at(
                pending.plan.long_entry.incarnation).projection_created_bar;
            admission::CommandEvent event;
            event.observation = std::move(observation);
            event.outcome = admission::Outcome::RejectedAffordability;
            adapter_.admission_journal.append(std::move(event));
            break;
        }
        case QualificationCase::IncarnationGap: {
            const auto original = pending.plan.final_short;
            no::RequestHandle replacement = original;
            replacement.incarnation += 10U;
            auto replacement_snapshot = placements.at(original.incarnation);
            CHECK(placements.find(replacement.incarnation) == placements.end());
            (adapter_.*access(RememberLiveTag{}))(replacement, std::move(replacement_snapshot));
            pending.plan.final_short = replacement;
            break;
        }
        case QualificationCase::FillBorn: {
            const auto handle = pending.plan.long_entry;
            auto row = placements.at(handle.incarnation);
            row.birth = OrderBirth::fill_evaluation(
                1, 2'000,
                BirthCursor::point(BirthCursorDomain::HistoricalPath, 1, 4),
                100.0, 1, 1, 2);
            row.birth_reach = compat::pine::HistoricalBirthReach::ExtremeWaypoints;
            // Birth/reach are admission-count operands. Rewrite the live
            // snapshot through the same subtract/install/add writer as a
            // replacement, preserving this guard's fill-born input.
            (adapter_.*access(RememberLiveTag{}))(handle, std::move(row));
            break;
        }
        case QualificationCase::NamedCancelRecreation:
            placements.at(pending.plan.long_entry.incarnation)
                .recreated_after_named_cancelled_entry_incarnation = 41;
            break;
        case QualificationCase::ReservationCapture:
            placements.at(pending.plan.materialize_long.incarnation)
                .reservation_expansion.capture(
                    pending.plan.materialize_long.incarnation,
                    pending.plan.seed_cycle, PositionSide::SHORT, pending.plan.seed_qty);
            break;
        case QualificationCase::MaterializeCarryMismatch:
            placements.at(pending.plan.materialize_long.incarnation)
                .projection_tv_carry_qty = 0.0;
            break;
        case QualificationCase::Baseline:
        case QualificationCase::NonHundredMargins:
            break;
        }
        qualified = (adapter_.*access(QualifyTag{}))(pending.plan);
        CHECK((adapter_.*access(LiveCountMismatchTag{}))() == -1);
        if (exercise_openings_) {
            // Use the same modeled next-open epoch as the qualification
            // guards, but also execute the production activation itself.
            const auto plan = pending.plan;
            const bool should_activate = which_ == QualificationCase::Baseline
                || which_ == QualificationCase::StreamPhase;
            (adapter_.*access(ActivatePlanTag{}))(NativeDecisionContext{});
            CHECK(!pending.ready);
            CHECK(adapter_.short_seed_collision_role_v1(plan.long_entry) == (should_activate ? 1 : 0));
            CHECK(adapter_.short_seed_collision_role_v1(plan.materialize_long) == (should_activate ? 2 : 0));
            CHECK(adapter_.short_seed_collision_role_v1(plan.final_short) == (should_activate ? 3 : 0));
            CHECK((adapter_.*access(LiveCountMismatchTag{}))() == -1);
            ++activations_checked;
            --epoch; // Let the following real host opening advance it normally.
        }
    }

private:
    QualificationCase which_;
    bool exercise_openings_;
};

void run_qualification_case(QualificationCase which, bool exercise_openings = false) {
    std::printf("qualification case %d\n", static_cast<int>(which));
    QualificationProbe host(which, exercise_openings);
    const Bar bars[] = {bar(60'000), bar(120'000), bar(180'000), bar(240'000)};
    const int count = exercise_openings ? 4 : 2;
    host.run(bars, count, "1", "1", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    CHECK(host.openings_checked == count);
    CHECK(host.activations_checked == (exercise_openings ? 1 : 0));
    if (which != QualificationCase::Magnifier
        && which != QualificationCase::NonHundredMargins) {
        CHECK(host.baseline);
    }
    const bool expected = which == QualificationCase::Baseline
        || which == QualificationCase::StreamPhase;
    CHECK(host.qualified == expected);
}

void unused_plan_projects_no_roles() {
    QualificationProbe host(QualificationCase::Baseline);
    const Bar bars[] = {bar(60'000), bar(120'000)};
    host.run(bars, 2, "1", "1", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    CHECK(host.roles_before_qualification_zero);
}

} // namespace

int main(int argc, char** argv) {
    const std::string selected = argc > 1 ? argv[1] : "all";
    if (selected == "all" || selected == "p0-10") {
        run_qualification_case(QualificationCase::Magnifier);
        run_qualification_case(QualificationCase::NonHundredMargins);
    }
    if (selected == "all" || selected == "p1-16")
        unused_plan_projects_no_roles();
    if (selected == "all" || selected == "p1-18") {
        for (const auto which : {
                 QualificationCase::StreamPhase,
                 QualificationCase::RejectedCommand,
                 QualificationCase::IncarnationGap,
                 QualificationCase::FillBorn,
                 QualificationCase::NamedCancelRecreation,
                 QualificationCase::ReservationCapture,
                 QualificationCase::MaterializeCarryMismatch,
             }) {
            run_qualification_case(which);
        }
    }
    if (selected == "all") run_qualification_case(QualificationCase::Baseline);
    if (selected == "all" || selected == "live-counts") {
        run_qualification_case(QualificationCase::Baseline, true);
        run_qualification_case(QualificationCase::FillBorn, true);
    }
    std::printf("L8c ShortSeed guards: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
