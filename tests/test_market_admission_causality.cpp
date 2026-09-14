// Literal API/allocator tests. No feed, generated strategy or grader is run.
#include "admission_literal_book.hpp"
#include <pineforge/market_admission.hpp>
#include <pineforge/compat/pine/market_admission.hpp>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace {
bool deny_allocation = false;
int failures = 0, checks = 0;
}
void* operator new(std::size_t size) {
    if (deny_allocation) throw std::bad_alloc();
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace pineforge::admission;
namespace {
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #condition); } } while (false)
template<class Fn> void refused(Fn&& fn) {
    bool rejected = false;
    try { fn(); } catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
}
std::shared_ptr<const CommandObservation> origin(uint64_t sequence, int bar = 10) {
    auto value = std::make_shared<CommandObservation>();
    value->command = sequence;
    value->bar = bar;
    return value;
}
CommandEvent command(uint64_t sequence, int bar = 10) {
    CommandEvent value;
    value.observation = origin(sequence, bar);
    return value;
}
uint64_t outstanding(const Journal& journal) {
    uint64_t value = 999;
    journal.reflect("journal", [&](const Field& field) {
        if (field.path == "journal.outstanding_sequences.size")
            value = std::get<uint64_t>(field.value);
    });
    return value;
}

void sequence_ownership() {
    Journal journal;
    const auto outer = journal.next_sequence();
    const auto inner = journal.next_sequence();
    CHECK(outstanding(journal) == 2);
    journal.append(command(inner));
    CHECK(outstanding(journal) == 1);
    journal.retain({});
    refused([&] { journal.append(command(inner)); });
    journal.append(command(outer)); // legal delayed outer completion
    CHECK(outstanding(journal) == 0);
    refused([&] { journal.append(command(outer)); });
    journal.retain({});
    refused([&] { journal.append(command(outer)); });
    refused([&] { journal.append(command(0)); });
    refused([&] { journal.append(command(journal.sequence_frontier())); });
    const auto abandoned = journal.next_sequence();
    journal.abandon(abandoned);
    refused([&] { journal.append(command(abandoned)); });
    CHECK(journal.sequence_frontier() == abandoned + 1);
    journal.reset();
    CHECK(journal.events().empty() && journal.sequence_frontier() == 1);
}

void allocation_failure_and_retry() {
    Journal journal;
    bool allocation_failed = false;
    deny_allocation = true;
    try { journal.reserve(); } catch (const std::bad_alloc&) { allocation_failed = true; }
    deny_allocation = false;
    CHECK(allocation_failed && journal.sequence_frontier() == 1 && outstanding(journal) == 0);
    auto allocation = journal.reserve();
    auto value = command(allocation.sequence());
    allocation_failed = false;
    deny_allocation = true;
    try { journal.append(value); } catch (const std::bad_alloc&) { allocation_failed = true; }
    deny_allocation = false;
    CHECK(allocation_failed && journal.events().empty() && outstanding(journal) == 1);
    journal.append(value);
    CHECK(journal.events().size() == 1 && outstanding(journal) == 0);
    // A completed handle still exists until its capture scope leaves. Reset
    // must not recycle its ID before that handle's destructor runs.
    refused([&] { journal.reset(); });
    refused([&] { Journal copy(journal); });
    refused([&] { Journal moved(std::move(journal)); });
    Journal destination;
    refused([&] { destination = journal; });
    refused([&] { destination = std::move(journal); });
    refused([&] { journal = destination; });
    refused([&] { journal = std::move(destination); });
    journal.retain({});
    refused([&] { journal.append(value); });
}

void capture_lifetimes() {
    Journal journal;
    int completed = 0;
    {
        auto allocation = journal.reserve();
        const auto id = allocation.sequence();
        CommandObservation input = *origin(id);
        CommandCapture outer(std::move(allocation), input, {}, [&](CommandEvent event) {
            ++completed; journal.append(std::move(event));
        });
        refused([&] { journal.reset(); });
        const auto nested = journal.next_sequence();
        journal.append(command(nested));
        journal.retain({});
        CHECK(outstanding(journal) == 1 && completed == 0);
    }
    CHECK(completed == 1 && outstanding(journal) == 0);
    {
        auto allocation = journal.reserve();
        const auto wrong = allocation.sequence() + 1;
        refused([&] {
            CommandCapture invalid(std::move(allocation), *origin(wrong), {}, [&](CommandEvent) { ++completed; });
        });
    }
    CHECK(outstanding(journal) == 0 && completed == 1);
    bool construction_failed = false;
    {
        auto allocation = journal.reserve();
        CommandObservation input = *origin(allocation.sequence());
        input.id.assign(256, 'x');
        deny_allocation = true;
        try {
            CommandCapture invalid(std::move(allocation), input, {}, [&](CommandEvent) { ++completed; });
        } catch (const std::bad_alloc&) { construction_failed = true; }
        deny_allocation = false;
    }
    CHECK(construction_failed && outstanding(journal) == 0 && completed == 1);
    int original_exception = 0;
    try {
        auto allocation = journal.reserve();
        const auto id = allocation.sequence();
        CommandCapture capture(std::move(allocation), *origin(id), {}, [&](CommandEvent) {
            ++completed; throw std::runtime_error("must not run while unwinding");
        });
        throw 71;
    } catch (int code) { original_exception = code; }
    CHECK(original_exception == 71 && outstanding(journal) == 0 && completed == 1);
    bool completion_failed = false;
    try {
        auto allocation = journal.reserve();
        const auto id = allocation.sequence();
        CommandCapture capture(std::move(allocation), *origin(id), {}, [&](CommandEvent) {
            throw std::runtime_error("completion failed");
        });
    } catch (const std::runtime_error&) { completion_failed = true; }
    CHECK(completion_failed && outstanding(journal) == 0);
    journal.reset();
    CHECK(journal.sequence_frontier() == 1);
    const auto unowned = journal.next_sequence();
    Journal copied(journal);
    copied.append(command(unowned));
    Journal moved(std::move(journal));
    moved.append(command(unowned));
    CHECK(journal.sequence_frontier() == 1 && outstanding(journal) == 0);
    CHECK(copied.events().size() == 1 && moved.events().size() == 1);
}

void receipt_identity_and_chronology() {
    Draft draft;
    refused([&] { draft.reviewed({101, Checkpoint::DefaultGross, 10, 100}); });
    refused([&] { draft.sizing_revised({101, 1, 10, 100}); });
    draft.bind(origin(100));
    for (uint64_t sequence : {0u, 99u, 100u}) {
        refused([&] { draft.reviewed({sequence, Checkpoint::DefaultGross, 10, 100}); });
        refused([&] { draft.sizing_revised({sequence, 1, 10, 100}); });
    }
    refused([&] { draft.reviewed({101, Checkpoint::DefaultGross, 9, 100}); });
    refused([&] { draft.reviewed({101, Checkpoint::DefaultGross, 10, 99}); });
    refused([&] { draft.reviewed({101, static_cast<Checkpoint>(99), 10, 100}); });
    CHECK(!draft.review());
    const ReviewReceipt first{101, Checkpoint::DefaultGross, 10, 100};
    draft.reviewed(first);
    draft.reviewed(first);
    refused([&] { draft.reviewed({101, Checkpoint::ExplicitPair, 10, 100}); });
    refused([&] { draft.reviewed({102, Checkpoint::DefaultGross, 11, 100}); });
    CHECK(draft.review()->sequence == 101 && draft.review()->target_command == 100);
    refused([&] { draft.sizing_revised({102, 1, 9, 100}); });
    refused([&] { draft.sizing_revised({102, 0, 10, 100}); });
    refused([&] { draft.sizing_revised({102, 1, 10, 99}); });
    const SizingRevision revision{102, 11, 12, 100};
    draft.sizing_revised(revision);
    draft.sizing_revised(revision);
    refused([&] { draft.sizing_revised({102, 12, 12, 100}); });
    refused([&] { draft.sizing_revised({101, 11, 12, 100}); });
    refused([&] { draft.sizing_revised({103, 11, 11, 100}); });
    refused([&] { draft.sizing_revised({103, 10, 12, 100}); });
    // One committed margin fill may cause both sizing and affordability refresh.
    draft.sizing_revised({103, 11, 12, 100});
    CHECK(draft.sizing_revision()->sequence == 103 && draft.sizing_revision()->cause_fill == 11);
}

void named_batch_review() {
    Journal journal;
    const auto original_sequence = journal.next_sequence();
    auto original = command(original_sequence);
    journal.append(original);
    Draft member; member.bind(original.observation);
    Draft foreign; foreign.bind(origin(original_sequence + 1));
    Draft copied; copied.bind(std::make_shared<const CommandObservation>(*original.observation));
    int completed = 0;
    {
        auto allocation = journal.reserve();
        ReviewEvent event;
        event.receipt = {allocation.sequence(), Checkpoint::ExplicitPair, 10};
        BookObservation book; book.incarnation = 17; book.draft = member;
        event.book.push_back(book); event.reviewed.push_back(book);
        ReviewCapture capture(std::move(allocation), event, [&](ReviewEvent completed_event) {
            ++completed; journal.append(std::move(completed_event));
        });
        refused([&] { capture.receipt_for(foreign); });
        Draft unbound; refused([&] { capture.receipt_for(unbound); });
        const auto receipt = capture.receipt_for(member);
        CHECK(capture.receipt_for(copied).target_command == receipt.target_command);
        CHECK(receipt.target_command == original_sequence && receipt.sequence > original_sequence);
        member.reviewed(receipt);
        refused([&] { journal.reset(); });
    }
    CHECK(completed == 1 && outstanding(journal) == 0);
    int caught = 0;
    try {
        auto allocation = journal.reserve();
        ReviewEvent event; event.receipt = {allocation.sequence(), Checkpoint::ExplicitPair, 10};
        ReviewCapture capture(std::move(allocation), event, [&](ReviewEvent) {
            ++completed; throw std::runtime_error("must not double throw");
        });
        throw 72;
    } catch (int code) { caught = code; }
    CHECK(caught == 72 && completed == 1 && outstanding(journal) == 0);
}

class ReflectedBook : public admission_test::Book {
public:
    Journal& journal() { return market_admission_journal(); }
};
Configuration default_scope() {
    Configuration c;
    c.pyramiding=1;
    c.default_quantity_type=1;
    c.default_quantity_value=100;
    c.long_margin=100;
    c.short_margin=100;
    c.risk_direction=0;
    return c;
}
std::shared_ptr<CommandObservation> observed(uint64_t command, int bar,
                                                    Configuration configuration = {}) {
    auto value=std::make_shared<CommandObservation>();
    value->command=command;value->bar=bar;value->configuration=configuration;
    value->prices={};value->requested_quantity=absent;value->oca_name.clear();
    return value;
}
BookObservation book_row(uint64_t incarnation, int bar,
                         std::shared_ptr<const CommandObservation> observation,
                         int placement_side=0) {
    BookObservation value;value.incarnation=incarnation;value.bar=bar;
    value.type=0;value.placement_side=placement_side;value.draft.bind(std::move(observation));
    return value;
}
void cross_bar_empty_review_retention(Checkpoint checkpoint, bool default_cause) {
    Journal journal;
    const auto live_command=journal.next_sequence();
    auto live_observation=observed(live_command,0,default_cause?default_scope():Configuration{});
    if(default_cause) {
        live_observation->original_sizing=SizingObservation{1,1000,100,100,1};
    }
    CommandEvent admitted;admitted.observation=live_observation;
    admitted.outcome=Outcome::Admitted;admitted.admitted_incarnation=7;
    journal.append(std::move(admitted));

    const auto cause_command=journal.next_sequence();
    auto cause_observation=observed(cause_command,1,default_cause?default_scope():Configuration{});
    cause_observation->requested_quantity=default_cause?absent:1;
    CommandEvent cause;cause.observation=cause_observation;
    cause.before.push_back(book_row(7,0,live_observation,default_cause?1:0));
    auto victim=observed(99,0,default_cause?default_scope():Configuration{});
    if(default_cause)victim->original_sizing=SizingObservation{1,1000,100,100,1};
    cause.before.push_back(book_row(8,0,victim,default_cause?1:0));
    cause.removed.push_back(8);
    journal.append(std::move(cause));

    uint64_t review_sequence=0;
    int completed=0;
    {
        // An empty review is still an actual checkpoint.  Exercise the
        // production RAII path rather than appending a synthetic event: its
        // destructor must commit the event before retention evaluates it.
        auto allocation=journal.reserve();
        review_sequence=allocation.sequence();
        ReviewEvent empty_review;empty_review.receipt={review_sequence,checkpoint,2};
        ReviewCapture capture(std::move(allocation),std::move(empty_review),
            [&](ReviewEvent event){++completed;journal.append(std::move(event));});
    }
    CHECK(completed==1 && journal.events().size()==3);

    const auto history=pineforge::compat::pine::admission_history(journal);
    if(default_cause) CHECK(history.default_causes.empty());
    else CHECK(history.pair_causes.empty());
    const auto retained=pineforge::compat::pine::admission_retention(journal,{7});
    CHECK(std::find(retained.begin(),retained.end(),cause_command)!=retained.end());
    CHECK(std::find(retained.begin(),retained.end(),review_sequence)!=retained.end());
    journal.retain(retained);
    // The producer survives as evidence, while the empty checkpoint still
    // clears the consumed domain. Keeping the review prevents resurrection.
    const auto after=pineforge::compat::pine::admission_history(journal);
    if(default_cause) CHECK(after.default_causes.empty());
    else CHECK(after.pair_causes.empty());
}
void empty_review_preserves_live_cross_bar_causes() {
    cross_bar_empty_review_retention(Checkpoint::DefaultGross,true);
    cross_bar_empty_review_retention(Checkpoint::TerminalGross,false);
}

void allocation_state_hash_and_values() {
    ReflectedBook book;
    const auto before = book.broker_state_hash();
    auto allocation = book.journal().reserve();
    const auto after = book.broker_state_hash();
    CHECK(before != after && outstanding(book.journal()) == 1);
    bool found = false;
    for (const auto& field : book.market_admission_fields())
        if (field.path == "journal.outstanding_sequences[0].sequence") {
            CHECK(std::get<uint64_t>(field.value) == allocation.sequence()); found = true;
        }
    CHECK(found);
    book.journal().abandon(allocation.sequence());
    CHECK(book.broker_state_hash() != after && outstanding(book.journal()) == 0);
}
}
int main() {
    sequence_ownership(); allocation_failure_and_retry(); capture_lifetimes();
    receipt_identity_and_chronology(); named_batch_review(); empty_review_preserves_live_cross_bar_causes(); allocation_state_hash_and_values();
    std::printf("admission causality: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
