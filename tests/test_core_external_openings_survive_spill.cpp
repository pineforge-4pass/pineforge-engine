// 1.7.0 review B1-02: a close bound to external openings keeps its
// membership when its book row is spilled or its accepted event is paged.
//
// WorkingRequestCore writes the newest book rows past its resident bound
// (kBookMax = 4,096, src/native_order.cpp) and its journal events past their
// resident bound (JournalStore::kMaxResident = 2,048) through one serializer.
// That serializer dropped external_openings from BindOpenings (the request's
// owner) and OpeningsClose (its authority), and the book's reload refused to
// take them from the resident definition: a close bound to three external
// openings came back bound to none, so observe_openings refused every
// observation of it and paged history showed an empty owner.
//
// The run (the core alone, its public C++ API): 4,095 resting rows, then the
// close bound to three external openings (the 4,096th row), then one more row,
// which spills the newest resident row -- the close. Its row must read back
// with the three openings, owner and authority; after more than 2,048 further
// events its accepted event, now paged, must too; and an allowance refresh
// that observes the three openings must be accepted for the spilled close.
//
// Previous-epoch API only, so this TU compiles against the unfixed head and
// fails there on the membership checks.
#include <pineforge/native_order.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <variant>

namespace {

namespace no = pineforge::native_order;
using Status = pineforge::detail::ScratchStatus;

int failures = 0;
int checks = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            ++failures;                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
        }                                                                           \
    } while (false)
#define REQUIRE(x)                                                                  \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            ++failures;                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
            throw std::runtime_error(#x);                                           \
        }                                                                           \
    } while (false)

constexpr std::size_t kResidentRows = 4096;     // kBookMax
constexpr std::size_t kResidentEvents = 2048;   // JournalStore::kMaxResident
constexpr std::uint64_t kOpenings = 3;

// Three live openings, handles 1..3 of the run, on cycle 7, long.
struct Openings final : no::OpeningObservationReadback {
    no::RunIdentity run;
    explicit Openings(const no::RunIdentity& value) : run(value) {}
    std::uint64_t count() const noexcept override { return kOpenings; }
    Status observation_at(std::uint64_t i, no::OpeningObservation& out) const noexcept override {
        if (i >= kOpenings) return Status::not_found;
        out = {{run, i + 1}, 7, no::PositionNonflat{7, no::Side::Long}, true};
        return Status::ok;
    }
    static Status read(void* context, std::uint64_t i, no::RequestHandle& out) {
        const auto& self = *static_cast<const Openings*>(context);
        if (i >= kOpenings) return Status::not_found;
        out = {self.run, i + 1};
        return Status::ok;
    }
};

// The membership a bound close reads back: three openings, handles 1..3.
bool holds_openings(const std::shared_ptr<const no::OpeningReadback>& external,
                    const no::RunIdentity& run) {
    if (!external || external->count() != kOpenings) return false;
    for (std::uint64_t i = 0; i != kOpenings; ++i) {
        no::RequestHandle got;
        if (external->opening_at(i, got) != Status::ok) return false;
        if (!(got == no::RequestHandle{run, i + 1})) return false;
    }
    return true;
}

no::Request resting() {
    no::Request request{no::Transact{1.0}, "rest", ""};
    request.trigger = no::Limit{50.0};
    return request;
}

struct Core {
    no::RunIdentity run{"b1-02-external-openings", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t incarnation = 1000;
    std::uint64_t ordinal = 1;

    // Every later command takes counters past everything committed.
    void advance() {
        if (incarnation <= core.last_incarnation()) incarnation = core.last_incarnation() + 1;
        if (ordinal <= core.last_ordinal()) ordinal = core.last_ordinal() + 1;
    }
    void submit_resting() {
        advance();
        REQUIRE(core.submit(resting(), 0, incarnation, ordinal).status
                == no::SubmitStatus::Accepted);
    }
};

void external_openings_survive_spill() {
    Core c;
    for (std::size_t i = 0; i + 1 != kResidentRows; ++i) c.submit_resting();

    // The close, bound to three external openings: the 4,096th row.
    Openings input(c.run);
    auto cursor = std::make_shared<no::OpeningInputCursor>(kOpenings, &input, Openings::read);
    no::Request close{no::Reduce{no::ExplicitUnits{1}}, "close", "external"};
    close.owner = no::BindOpenings{{}, 7, cursor};
    no::CommandContext command;
    command.external_openings = &input;
    c.advance();
    const auto installed = c.core.apply_submit(close, command, c.incarnation, c.ordinal);
    REQUIRE(installed.result.status == no::SubmitStatus::Accepted && installed.result.handle);
    const no::RequestHandle handle = *installed.result.handle;
    const std::size_t accepted_at = installed.events.first_index;
    {
        // Resident, the membership is there (a precondition, not the defect).
        const no::LiveRequest* row = c.core.find_live(handle);
        REQUIRE(row != nullptr);
        REQUIRE(holds_openings(std::get<no::BindOpenings>(row->request().owner).external_openings,
                               c.run));
    }

    // One more row: the book is past its resident bound and the newest
    // resident row, the close, is spilled.
    c.submit_resting();
    REQUIRE(c.core.live().size() == kResidentRows + 1);
    {
        const no::LiveRequest* row = c.core.find_live(handle);
        REQUIRE(row != nullptr);
        const auto* owner = std::get_if<no::BindOpenings>(&row->request().owner);
        const auto* authority = std::get_if<no::OpeningsClose>(&row->authority);
        REQUIRE(owner != nullptr && authority != nullptr);
        CHECK(holds_openings(owner->external_openings, c.run));
        CHECK(holds_openings(authority->external_openings, c.run));
    }

    // More than the journal's resident bound of events after the close's
    // acceptance: that event is read back from a page.
    for (std::size_t i = 0; i != kResidentEvents + 64; ++i) c.submit_resting();
    REQUIRE(c.core.history_end() - accepted_at > kResidentEvents);
    {
        const no::CommandEvent& event = c.core.history_at(accepted_at);
        const auto* accepted = std::get_if<no::AcceptedEvent>(&event);
        REQUIRE(accepted != nullptr && accepted->definition);
        REQUIRE(accepted->definition->handle == handle);
        const auto* owner = std::get_if<no::BindOpenings>(&accepted->definition->request.owner);
        REQUIRE(owner != nullptr);
        CHECK(holds_openings(owner->external_openings, c.run));
    }

    // The spilled close accepts an observation of its three openings.
    c.advance();
    no::EvaluationContext evaluation;
    evaluation.cursor.point.ordinal = c.ordinal + 16;
    evaluation.cursor.point.effective_time_ms = 1000;
    evaluation.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
    evaluation.driver_class = no::DriverEligibilityClass::ObservedPrint;
    evaluation.existing_matching_bit = true;
    no::TargetObservation observed;
    observed.current_position = no::PositionNonflat{7, no::Side::Long};
    observed.external_openings = &input;
    CHECK(c.core.refresh_allowance(handle, evaluation, observed));
}

}  // namespace

int main() {
    try {
        external_openings_survive_spill();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "external openings survive spill: %s\n", error.what());
    }
    std::printf("test_core_external_openings_survive_spill: %d checks, %d failures\n", checks,
                failures);
    return failures == 0 ? 0 : 1;
}
