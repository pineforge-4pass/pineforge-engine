#include "book_allowance_core_fixture.hpp"

#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <variant>

namespace {
using namespace book_allowance_core;

struct Case {
    const char* name;
    no::RequestHandle handle;
    no::LiveRequest before;
    no::EvaluationContext eval = context();
    no::TargetObservation observation{};
    bool timeline = true;
    bool succeeds = false;
    bool binds = false;
};

void branch_matrix(bool spilled) {
    Fixture f(spilled);
    std::vector<Case> cases;
    const no::RequestHandle opening{f.core.identity(), 1};
    const auto seed = [&](const char* name, const no::Authority& authority,
                          bool succeeds = false, bool kept = false) -> Case& {
        const auto handle = f.submit();
        auto& live = f.newest();
        live.authority = authority;
        live.allowance = no::AllowanceUnits{old_point, 0.75, 0.25};
        if (kept) {
            auto definition = std::make_shared<no::RequestDefinition>(*live.definition);
            definition->kept_binding = no::PositionNonflat{7, no::Side::Long};
            live.definition = std::move(definition);
        }
        cases.push_back({name, handle, live});
        cases.back().succeeds = succeeds;
        return cases.back();
    };
    const auto set_row = [&](Case& row, const std::function<void(no::LiveRequest&)>& edit) {
        edit(f.newest());
        row.before = f.newest();
    };
    seed("ordinary units", no::BookTransaction{}, true);
    set_row(seed("unset grows once", no::BookTransaction{}, true),
            [](auto& row) { row.allowance = no::AllowanceUnset{}; });
    set_row(seed("same point", no::BookTransaction{}),
            [](auto& row) { row.allowance = no::AllowanceUnits{new_point, 0.75, 0.25}; });
    seed("waiting", no::Wait{opening});
    auto& birth = seed("birth time ineligible", no::BookTransaction{});
    set_row(birth, [](auto& row) {
        auto definition = std::make_shared<no::RequestDefinition>(*row.definition);
        definition->birth.decision_time_lower_bound = 2000;
        row.definition = std::move(definition);
    });
    seed("birth ordinal ineligible", no::BookTransaction{}).eval.cursor.point.ordinal = 1;
    seed("driver ineligible", no::BookTransaction{}).eval.driver_class =
        no::DriverEligibilityClass::CurrentExecution;
    seed("cohort missing side", no::CohortClose{no::CohortHandle{1}});
    seed("cohort no live member", no::CohortClose{no::CohortHandle{1}})
        .eval.cohort_side = no::Side::Long;
    auto& cohort = seed("cohort alive", no::CohortClose{no::CohortHandle{1}}, true);
    cohort.eval.cohort_side = no::Side::Long;
    cohort.observation.openings.push_back(
        {opening, 7, no::PositionNonflat{7, no::Side::Long}, true});
    const no::BookClose book{7, no::Side::Long, {}, {}};
    seed("book flat", book);
    seed("book wrong cycle", book).observation.current_position =
        no::PositionNonflat{8, no::Side::Long};
    seed("book wrong side", book).observation.current_position =
        no::PositionNonflat{7, no::Side::Short};
    seed("book alive", book, true).observation.current_position =
        no::PositionNonflat{7, no::Side::Long};
    const no::OpeningClose one{opening, 7, no::Side::Long, no::EnrollmentFromCommand{}};
    seed("opening missing", one);
    auto& dead = seed("opening dead", one);
    dead.observation.current_position = no::PositionNonflat{7, no::Side::Long};
    dead.observation.opening = no::OpeningObservation{
        opening, 7, dead.observation.current_position, false};
    auto& alive = seed("opening alive", one, true);
    alive.observation.current_position = no::PositionNonflat{7, no::Side::Long};
    alive.observation.opening = no::OpeningObservation{
        opening, 7, alive.observation.current_position, true};
    no::OpeningsClose many;
    many.openings = {opening};
    many.cycle = 7;
    many.side = no::Side::Long;
    seed("openings missing observation", many);
    auto& empty = seed("openings none alive", many);
    empty.observation.current_position = no::PositionNonflat{7, no::Side::Long};
    empty.observation.openings = {
        {opening, 7, empty.observation.current_position, false}};
    auto& selected = seed("openings alive", many, true);
    selected.observation.current_position = no::PositionNonflat{7, no::Side::Long};
    selected.observation.openings = {
        {opening, 7, selected.observation.current_position, true}};
    seed("unbound no carried binding", no::UnboundBookClose{});
    seed("unbound no timeline", no::UnboundBookClose{}, false, true).timeline = false;
    seed("kept flat", no::UnboundBookClose{}, false, true);
    seed("kept invalid cycle", no::UnboundBookClose{}, false, true)
        .observation.current_position = no::PositionNonflat{0, no::Side::Long};
    seed("kept changed cycle", no::UnboundBookClose{}, false, true)
        .observation.current_position = no::PositionNonflat{8, no::Side::Long};
    auto& same = seed("kept same point", no::UnboundBookClose{}, false, true);
    same.observation.current_position = no::PositionNonflat{7, no::Side::Long};
    set_row(same, [](auto& row) {
        row.allowance = no::AllowanceUnits{new_point, 0.75, 0.25};
    });
    auto& kept = seed("kept binds", no::UnboundBookClose{}, true, true);
    kept.observation.current_position = no::PositionNonflat{7, no::Side::Long};
    kept.binds = true;
    set_row(seed("all-scope", no::BookTransaction{}, true), [](auto& row) {
        row.remaining = no::RemainingFlattenAll{};
        row.allowance = no::AllowanceAllScope{old_point};
    });
    set_row(seed("deferred", no::BookTransaction{}, true), [](auto& row) {
        row.remaining = no::RemainingDeferred{};
        row.allowance = no::AllowanceDeferred{old_point};
    });

    const auto sentinel = f.submit(); // The final selected row spills too.
    const auto generation = f.generation();
    for (auto& row : cases) {
        std::printf("%s: %s\n", spilled ? "spilled" : "resident", row.name);
        require(f.resident(row.handle) != spilled, "branch uses the intended physical row");
        const auto at = f.locator(row.handle);
        require((at != 0) == spilled, "scan locator proves spilled branch coverage");
        const auto before = f.core.book_refresh_work();
        // Preparing a different resident row cannot promote the selected one.
        auto token = f.core.prepare_cancel(sentinel, f.ordinal);
        const auto epoch = f.epoch();
        const auto ordinal = f.ordinal;
        const auto last = f.core.last_ordinal();
        const auto history = f.core.history_end();
        const bool changed = f.refresh(row.handle, row.eval, row.observation,
                                       row.timeline, at, generation);
        require(changed == row.succeeds, "resident and decoded branch verdicts agree");
        auto expected = row.before;
        if (changed) {
            if (std::holds_alternative<no::RemainingFlattenAll>(expected.remaining)) {
                expected.allowance = no::AllowanceAllScope{new_point};
            } else if (std::holds_alternative<no::RemainingDeferred>(expected.remaining)) {
                expected.allowance = no::AllowanceDeferred{new_point};
            } else {
                expected.allowance = no::AllowanceUnits{new_point, 0.75, 0.75};
            }
            if (row.binds) {
                expected.authority = no::BookClose{7, no::Side::Long,
                    no::EventId{f.core.identity(), ordinal}, row.eval.cursor};
            }
        }
        require(folded(*f.core.hold_live(row.handle)) == folded(expected),
                "every logical field matches the expected row after refresh");
        require(f.core.last_ordinal() == (changed && row.binds ? ordinal : last),
                "only a successful kept binding publishes its ordinal");
        require(f.ordinal == ordinal && f.core.history_end() == history,
                "refresh consumes no caller counter and appends no event");
        require(f.epoch() > epoch || !changed, "each successful refresh advances the epoch");
        require(changed || f.epoch() == epoch, "false refresh leaves the epoch unchanged");
        if (changed) {
            const auto installed = f.core.install_cancel(std::move(token));
            require(std::holds_alternative<no::InstallError>(installed)
                && std::get<no::InstallError>(installed) == no::InstallError::StalePreparation,
                "no token prepared before a refresh installs after it");
        }
        const auto after = f.core.book_refresh_work();
        no_churn(after);
        require(after.keyed_searches == before.keyed_searches,
                "validated scan refresh performs no keyed search");
        const auto writes = after.in_place_writes + after.appended_writes
                          - before.in_place_writes - before.appended_writes;
        require(writes == (changed && spilled ? 1U : 0U),
                "false/no-op refresh has zero writes; changed spilled row has one publication");
        require(f.generation() == generation, "refresh never compacts held scan locators");
        if (changed && row.binds) f.ordinal = f.core.last_ordinal() + 1;
    }
}

void locator_fallbacks() {
    Fixture f;
    const auto first = f.submit();
    f.newest().allowance = no::AllowanceUnits{old_point, 0.75, 0.25};
    const auto second = f.submit();
    f.newest().allowance = no::AllowanceUnits{old_point, 0.75, 0.25};
    f.submit();
    const auto at = f.locator(first);
    const auto other_at = f.locator(second);
    const auto gen = f.generation();
    require(f.refresh(first, context(), {}, true, other_at, gen),
            "mismatched incarnation falls back to the current row");
    require(f.refresh(second, context(), {}, true, at, gen + 1),
            "mismatched generation falls back to the current row");
    const auto wrong_run = no::RequestHandle{{"another-run", 1}, first.incarnation};
    const auto epoch = f.epoch();
    const auto before = f.core.book_refresh_work();
    require(!f.refresh(wrong_run, context(), {}, true, at, gen), "foreign handle is refused");
    require(!f.refresh({f.core.identity(), 999999}, context(), {}, true, at, gen),
            "absent handle is refused");
    const auto after = f.core.book_refresh_work();
    no_churn(after);
    require(f.epoch() == epoch && after.in_place_writes == before.in_place_writes
        && after.appended_writes == before.appended_writes, "invalid refreshes write nothing");

    // The public, handle-only entry has identical invalidation semantics.
    Fixture resident(false);
    const auto target = resident.submit();
    auto token = resident.core.prepare_cancel(target, resident.ordinal);
    require(resident.core.refresh_allowance(target, context(), {}),
            "public resident refresh succeeds");
    const auto stale = resident.core.install_cancel(std::move(token));
    require(std::holds_alternative<no::InstallError>(stale)
        && std::get<no::InstallError>(stale) == no::InstallError::StalePreparation,
        "public refresh also invalidates existing preparations");
}
} // namespace

int main() {
    try {
        branch_matrix(false);
        branch_matrix(true);
        locator_fallbacks();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "book allowance refresh: %s\n", error.what());
        return 1;
    }
}
