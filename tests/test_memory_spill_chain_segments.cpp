// Long-chain backing growth, resident tails and reset lifetimes are checked.
// Include the actual implementation to complete its
// implementation-only storage types. Private members are read with explicit
// instantiation, without a product seam, layout mirror, or access-level macro.
// These checks characterize growth and retirement, not a cap on retained bytes.
#include "../src/native_order.cpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace memory_spill_chain_segments {
namespace no = pineforge::native_order;
using Word = std::uint64_t;
using Core = no::WorkingRequestCore;
using Handle = no::RequestHandle;
using Segment = std::tuple<Word, Word, Word>;
using SegmentLog = no::TailLog<no::ChainSegmentRec, 16>;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Segments { friend auto access(Segments); };
struct IndexFile { friend auto access(IndexFile); };
template struct Access<Segments, &Core::chain_segments_>;
template struct Access<IndexFile, &SegmentLog::file_>;

std::size_t checks = 0, failures = 0;
std::string trial;
struct PrerequisiteFailure final : std::exception {};

bool check(bool passed, const char* expression, int line) {
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL [chain-segments] %s:%d: %s (trial=%s)\n",
                     __FILE__, line, expression, trial.c_str());
    }
    return passed;
}
#define CHECK(expression) check(bool(expression), #expression, __LINE__)
#define NEED(expression) do { \
    if (!check(bool(expression), "fixture prerequisite: " #expression, __LINE__)) \
        throw PrerequisiteFailure{}; \
} while (false)

template<class Action>
void run(const std::string& name, Action&& action) {
    trial = name;
    try {
        action();
    } catch (const PrerequisiteFailure&) {
        // Only an unsafe fixture prerequisite ends this trial. Other trials run.
    } catch (const std::exception& error) {
        check(false, error.what(), __LINE__);
    } catch (...) {
        check(false, "unknown exception", __LINE__);
    }
}

no::EvaluationContext point(Word ordinal) {
    no::EvaluationContext context;
    context.cursor.point.ordinal = ordinal;
    context.cursor.point.effective_time_ms = 1000;
    context.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
    context.driver_class = no::DriverEligibilityClass::ObservedPrint;
    context.existing_matching_bit = true;
    return context;
}

struct Book {
    no::RunIdentity identity{"chain-segments", 1};
    Core core{identity};
    Word incarnation = 1, ordinal = 1;
    bool filled = false;

    Handle submit(no::Request request) {
        const Handle expected{identity, incarnation};
        const auto result = core.submit(request, 1000, incarnation, ordinal);
        NEED(result.status == no::SubmitStatus::Accepted && result.handle);
        CHECK(*result.handle == expected);
        return *result.handle;
    }
    void installed(no::Preparation<no::Installed> result) {
        NEED(std::holds_alternative<no::Installed>(result));
        const auto count = std::get<no::Installed>(result).events.count;
        CHECK(count == 1);
        ordinal += count;
    }
    void install(no::Preparation<no::PreparedMutation> preparation,
                 std::size_t expected_events = 1) {
        NEED(std::holds_alternative<no::PreparedMutation>(preparation));
        auto result = core.install_mutation(
            std::get<no::PreparedMutation>(std::move(preparation)));
        NEED(std::holds_alternative<no::Installed>(result));
        const auto count = std::get<no::Installed>(result).events.count;
        CHECK(count == expected_events);
        ordinal += count;
    }
    no::EventId fill(Handle target, double units) {
        const no::PositionIdentity before = filled
            ? no::PositionIdentity{no::PositionNonflat{1, no::Side::Long}}
            : no::PositionIdentity{no::PositionFlat{}};
        const auto context = point(ordinal);
        no::TargetObservation observation;
        observation.current_position = before;
        auto evaluated = core.prepare_evaluation(target, context, observation, ordinal);
        if (std::holds_alternative<no::PreparedMutation>(evaluated))
            // An Independent Transact refreshes allowance without a receipt.
            install(std::move(evaluated), 0);
        else
            NEED(std::holds_alternative<no::NoChange>(evaluated));
        no::ExecutionProposal proposal;
        proposal.cursor = context.cursor;
        proposal.raw_price = proposal.resolved_price = 100.0;
        proposal.physical_action = no::Transact{units};
        proposal.pre_fill = before;
        proposal.pre_target.current_position = before;
        proposal.inspected_opened_units = units;
        proposal.inspected_current_ticket = 1;
        auto prepared = core.prepare_execution(target, proposal, ordinal);
        NEED(std::holds_alternative<no::PreparedExecution>(prepared));
        const no::EventId expected{identity, ordinal};
        no::CommittedExecutionFacts facts;
        facts.result.status = pineforge::execution::Status::Applied;
        facts.result.opened_units = units;
        facts.result.current_ticket = 1;
        facts.cycle_before = filled ? 1 : 0;
        facts.cycle_after = 1;
        facts.post_target.current_position = no::PositionNonflat{1, no::Side::Long};
        facts.committed_action = no::Transact{units};
        auto result = core.install_execution(
            std::get<no::PreparedExecution>(std::move(prepared)), facts);
        NEED(std::holds_alternative<no::Installed>(result));
        CHECK(std::get<no::Installed>(result).events.count == 1);
        ordinal += std::get<no::Installed>(result).events.count;
        const auto* event = std::get_if<no::ExecutionAppliedEvent>(&core.history().back());
        NEED(event);
        CHECK(event->ordinal == expected.ordinal && event->handle() == target
              && event->terminal && event->filled_working == units);
        filled = true;
        return expected;
    }
    Word effect(no::EventId cause, Handle recipient, bool direct) {
        // The reference model records our next command ordinal before commit;
        // it never obtains expected ids from chain_segments_ or the journal.
        const Word expected = ordinal;
        if (direct) installed(core.apply_group_effect(cause, recipient, ordinal));
        else install(core.prepare_group_effect(cause, recipient, ordinal));
        CHECK(ordinal == expected + 1);
        return expected;
    }
    Word resolve(Handle target, bool direct) {
        no::TermsResolvedInput input;
        input.price_kind = no::NativeCandidatePriceKind::PointPrice;
        input.raw_price = input.default_resolved_price = 100.0;
        input.terms.resolved_price = 100.0;
        input.terms.units = 1000000.0;
        input.terms.shape = no::OpeningShape::Transact;
        const Word expected = ordinal;
        const auto context = point(ordinal);
        if (direct) installed(core.apply_terms(target, context, input, ordinal));
        else install(core.prepare_terms(target, context, input, ordinal));
        CHECK(ordinal == expected + 1);
        return expected;
    }
    void cancel_and_retire(Handle target) {
        NEED(core.cancel(target, ordinal).status == no::CancelStatus::Cancelled);
        CHECK(core.live_size() == 0);
        const auto retained = core.history().size();
        CHECK(retained != 0);
        CHECK(core.retire_history(core.last_ordinal()) == retained);
        CHECK(core.history().empty());
    }
};

// Source arithmetic: TailWindowConfig defaults to C=16 index records; a full
// TailLog spills its older C/2=8 before the next push. Nothing retires this log.
// This calculation uses neither observed capacity nor production helpers.
Word flushed_rows(Word resolutions) {
    return resolutions <= 16 ? 0 : ((resolutions - 16 + 7) / 8) * 8;
}

void observe(Book& book, const std::vector<Word>& ids,
             const std::vector<Segment>& expected) {
    const auto& store = book.core.*access(Segments{});
    CHECK(store && store->ids.size() == ids.size() * sizeof(Word)
          && store->index.size() == expected.size());
    if (!store) return; // A missing spill is already a failure; keep dispatching.
    const Word flushed = flushed_rows(expected.size());
    CHECK(store->index.base() == 0 && store->index.retained() == expected.size());
    CHECK(store->index.capacity() == 16
          && store->index.resident() == expected.size() - flushed
          && store->index.resident() <= 16);
    // ChainSegmentRec has three uint64 leaves. sizeof verifies the layout
    // assumption separately; expected file bytes do not read sizeof(Record).
    CHECK(sizeof(no::ChainSegmentRec) == 3 * sizeof(Word));
    CHECK((store->index.*access(IndexFile{})).size() == flushed * 3 * sizeof(Word));
    CHECK(store->ids.size() % sizeof(Word) == 0);
    std::vector<Word> actual_ids(static_cast<std::size_t>(store->ids.size() / sizeof(Word)));
    store->ids.read(0, actual_ids.data(), actual_ids.size() * sizeof(Word));
    CHECK(actual_ids == ids);
    std::vector<Segment> actual;
    for (Word i = store->index.base(); i < store->index.size(); ++i) {
        const auto record = store->index.get(i);
        actual.emplace_back(record.event_ordinal, record.offset, record.count);
    }
    CHECK(actual == expected);
}

struct ResolvedChain {
    Handle target;
    Word resolution;
    std::vector<Word> ids;
};

ResolvedChain make_chain(Book& book, Word links, bool direct) {
    no::Request target{no::HostSized{no::HostSizedKind::Open, no::Side::Long}, "chain-target", ""};
    target.group = no::Member{7, 2, no::GroupEffect::Reduce};
    ResolvedChain chain{book.submit(std::move(target)), 0, {}};
    for (Word i = 0; i != links; ++i) {
        no::Request source{no::Transact{0.25}, "chain-source", ""};
        source.group = no::Member{7, 1, no::GroupEffect::Reduce};
        const auto cause = book.fill(book.submit(std::move(source)), 0.25);
        chain.ids.push_back(book.effect(cause, chain.target, direct));
    }
    const auto* live = book.core.find_live(chain.target);
    NEED(live);
    const auto* pending = std::get_if<no::PendingDeferred>(&live->pending);
    NEED(pending);
    CHECK(pending->count == links && pending->total == 0.25 * links);
    chain.resolution = book.resolve(chain.target, direct);
    const auto* event = book.core.event_at({book.identity, chain.resolution});
    NEED(event);
    const auto* terms = std::get_if<no::TermsResolvedEvent>(event);
    NEED(terms);
    CHECK(terms->pending_total == 0.25 * links);
    CHECK(terms->prior_adjustment_ids.size() == (links <= 256 ? links : 0));
    CHECK(book.core.prior_adjustment_count({book.identity, chain.resolution}) == links);
    std::vector<Word> visited;
    bool same_run = true;
    CHECK(book.core.visit_prior_adjustments({book.identity, chain.resolution},
        [&](const no::EventId& id) {
            same_run = same_run && id.run == book.identity;
            visited.push_back(id.ordinal);
            return true;
        }));
    CHECK(same_run && visited == chain.ids);
    return chain;
}

void threshold(Word links, bool direct) {
    Book book;
    const auto chain = make_chain(book, links, direct);
    if (links <= 256) {
        const auto& store = book.core.*access(Segments{});
        CHECK(!store || (store->ids.size() == 0 && store->index.size() == 0));
        book.cancel_and_retire(chain.target);
        CHECK(!store || (store->ids.size() == 0 && store->index.size() == 0));
    } else {
        const std::vector<Segment> expected{{chain.resolution, 0, links}};
        observe(book, chain.ids, expected);
        book.cancel_and_retire(chain.target);
        observe(book, chain.ids, expected);
    }
}

void growth(Word chains, bool direct) {
    Book book;
    std::vector<Word> ids;
    std::vector<Segment> expected;
    // 513 is greater than the 256 inline threshold and the 512-word I/O
    // blocks in collect_pending_chain. 17 and 68 both exceed the index tail.
    constexpr Word links = 513;
    for (Word i = 0; i != chains; ++i) {
        const auto chain = make_chain(book, links, direct);
        const Word offset = ids.size() * sizeof(Word);
        ids.insert(ids.end(), chain.ids.begin(), chain.ids.end());
        expected.emplace_back(chain.resolution, offset, links);
        observe(book, ids, expected);
        book.cancel_and_retire(chain.target);
        // Window-style journal retirement does run, but chain storage has no in-run
        // retire site. Both complete containers must still equal the model.
        observe(book, ids, expected);
    }
    const auto& store = book.core.*access(Segments{});
    CHECK(store && store->ids.size() == chains * links * sizeof(Word)
          && store->index.size() == chains
          && (store->index.*access(IndexFile{})).size() != 0);
    // No owning test copy: reset must release this run's actual storage.
    std::weak_ptr old = store;
    book.core.reset({"chain-segments", 2});
    CHECK(old.expired() && !(book.core.*access(Segments{})));
}
} // namespace memory_spill_chain_segments

int main() {
    using namespace memory_spill_chain_segments;
    for (bool direct : {false, true}) {
        const std::string mode = direct ? "direct" : "staged";
        for (Word links : {Word{256}, Word{257}})
            run(mode + "-threshold-" + std::to_string(links),
                [=] { threshold(links, direct); });
        for (Word chains : {Word{17}, Word{68}})
            run(mode + "-growth-" + std::to_string(chains),
                [=] { growth(chains, direct); });
    }
    std::printf("memory proof spill_chain_segments: %zu checks, %zu failures\n", checks, failures);
    return failures ? 1 : 0;
}
