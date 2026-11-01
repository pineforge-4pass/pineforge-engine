// Command-journal backing growth, resident tails and retirement are checked.
// Include the implementation verbatim to complete the
// actual JournalStore and its anonymous-namespace TailLog/ExtFile types.
// No product seam, access-level macro, layout mirror, or copied core is used.
#include "../src/native_order.cpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace memory_spill_journal {
namespace no = pineforge::native_order;
using Word = std::uint64_t;
using Core = no::WorkingRequestCore;
using Handle = no::RequestHandle;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Journal { friend auto access(Journal); };
struct PageFile { friend auto access(PageFile); };
template struct Access<Journal, &Core::journal_>;
using PageLog = no::TailLog<no::JournalPage, 16>;
template struct Access<PageFile, &PageLog::file_>;

std::size_t checks = 0, failures = 0;
const char* trial = "startup";
bool check(bool passed, const char* expression, int line) {
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL [journal] %s:%d: %s (trial=%s)\n",
                     __FILE__, line, expression, trial);
    }
    return passed;
}
#define CHECK(expression) check(bool(expression), #expression, __LINE__)

template<class Action>
void run(const char* name, Action&& action) {
    trial = name;
    try {
        action();
    } catch (const std::exception& error) {
        check(false, error.what(), __LINE__);
    } catch (...) {
        check(false, "unknown exception", __LINE__);
    }
}

// Source arithmetic, independent of the inspected containers:
// JournalStore admits 2048 resident events, then pages a chunk of at most 32.
// This fixture's 32 short events fit in the separate 32768-byte page limit.
// TailLog<JournalPage,16> writes its older 8 records when a 17th is incoming.
constexpr Word resident_limit = 2048;
constexpr Word chunk_events = 32;
constexpr Word page_tail = 16;
constexpr Word page_limit = 32768;
const std::string label = "command-journal";
const std::string comment = "accepted-and-cancelled";

no::Request request() {
    no::Request result{no::Transact{1.0}, label, comment};
    result.trigger = no::Limit{90.0};
    return result;
}

// Count the serialized fields in archive_io(Request/RequestDefinition/
// AcceptedEvent/CancelledEvent), without calling any production serializer or
// reading a page length. Variants have a 4-byte tag, strings an 8-byte length,
// optionals/definition references a 1-byte marker; trivial leaves use sizeof.
// A user cancellation has a present cause, BookTransaction, unexecuted units,
// PendingNone. Each definition has absent predecessor/root/kept_binding.
std::pair<Word, Word> event_bytes() {
    static_assert(std::is_trivially_copyable_v<Handle>);
    static_assert(std::is_trivially_copyable_v<no::Birth>);
    static_assert(std::is_trivially_copyable_v<no::EventId>);
    constexpr Word tag = sizeof(std::uint32_t), word = sizeof(Word), marker = 1;
    const Word request_bytes = tag + sizeof(no::Transact)
        + word + label.size() + word + comment.size()
        + tag + sizeof(no::Limit)
        + tag + sizeof(no::ImmediateRemaining)
        + tag + sizeof(no::Independent)
        + tag + sizeof(no::NoGroup)
        + tag + sizeof(no::Absolute);
    const Word definition = marker + sizeof(Handle) + request_bytes
        + sizeof(no::Birth) + marker + sizeof(no::RequestOrigin)
        + marker + word + marker;
    const Word accepted = tag + word + definition + sizeof(no::CommandSurface);
    const Word cancelled = tag + word + definition + sizeof(no::CancelReason)
        + marker + sizeof(no::EventId)
        + tag + sizeof(no::BookTransaction)
        + tag + sizeof(no::RemainingProjectionUnits)
        + tag + sizeof(no::PendingNone);
    return {accepted, cancelled};
}

Word expected_pages(Word events) {
    return events <= resident_limit ? 0
        : (events - resident_limit + chunk_events - 1) / chunk_events;
}
Word flushed_page_rows(Word pages) {
    const Word half = page_tail / 2;
    return pages <= page_tail ? 0
        : ((pages - page_tail + half - 1) / half) * half;
}

struct Book {
    no::RunIdentity identity{"journal", 1};
    Core core{identity};
    Word incarnation = 1, ordinal = 1, pairs = 0;

    void grow_to(Word target) {
        for (; pairs < target; ++pairs) {
            const Handle expected{identity, pairs + 1};
            const Word accepted_ordinal = 2 * pairs + 1;
            const auto accepted = core.submit(request(), 0, incarnation, ordinal);
            CHECK(accepted.status == no::SubmitStatus::Accepted);
            CHECK(accepted.handle && *accepted.handle == expected
                  && accepted.event_ordinal == accepted_ordinal);
            // An absent handle is already a failure. Avoid dereferencing it;
            // later population assertions still run and expose the mismatch.
            if (accepted.handle) {
                const auto cancelled = core.cancel(*accepted.handle, ordinal);
                CHECK(cancelled.status == no::CancelStatus::Cancelled
                      && cancelled.event_ordinal == accepted_ordinal + 1);
            }
        }
        CHECK(incarnation == target + 1 && ordinal == 2 * target + 1);
    }
};

bool definition_matches(const no::DefinitionRef& definition, const Book& book,
                        Word position) {
    if (!definition) return false;
    const auto& value = definition->request;
    const auto* units = std::get_if<no::Transact>(&value.intent);
    const auto* limit = std::get_if<no::Limit>(&value.trigger);
    return definition->handle == Handle{book.identity, position / 2 + 1}
        && definition->birth.acceptance_ordinal == 2 * (position / 2) + 1
        && definition->birth.decision_time_lower_bound == 0
        && definition->origin == no::RequestOrigin::Host
        && !definition->predecessor && !definition->root
        && definition->priority == 0 && !definition->kept_binding
        && units && units->signed_units == 1.0 && limit
        && limit->price == 90.0 && !limit->fill_through
        && value.label == label && value.comment == comment
        && std::holds_alternative<no::ImmediateRemaining>(value.capacity)
        && std::holds_alternative<no::Independent>(value.owner)
        && std::holds_alternative<no::NoGroup>(value.group)
        && std::holds_alternative<no::Absolute>(value.anchor);
}

void verify_payload(Book& book, Word first_position, Word events) {
    CHECK(book.core.history_base() == first_position
          && book.core.history_end() == events);
    bool equal = true;
    // Use the actual valid domain for safe diagnostics if retirement is wrong.
    // The domain equality above is an independent assertion.
    const Word begin = std::max<Word>(first_position, book.core.history_base());
    const Word end = std::min<Word>(events, book.core.history_end());
    for (Word position = begin; position < end; ++position) {
        const auto& event = book.core.history_at(position);
        if (position % 2 == 0) {
            const auto* accepted = std::get_if<no::AcceptedEvent>(&event);
            const bool matches = accepted && accepted->ordinal == position + 1
                && accepted->surface == no::CommandSurface::General
                && definition_matches(accepted->definition, book, position);
            equal = matches && equal;
        } else {
            const auto* cancelled = std::get_if<no::CancelledEvent>(&event);
            const auto* unexecuted = cancelled
                ? std::get_if<no::RemainingProjectionUnits>(&cancelled->unexecuted) : nullptr;
            const bool matches = cancelled && cancelled->ordinal == position + 1
                && definition_matches(cancelled->definition, book, position)
                && cancelled->reason == no::CancelReason::User
                && cancelled->cause
                && *cancelled->cause == (no::EventId{book.identity, position + 1})
                && std::holds_alternative<no::BookTransaction>(cancelled->prior_authority)
                && unexecuted && unexecuted->q == 1.0
                && std::holds_alternative<no::PendingNone>(cancelled->pending);
            equal = matches && equal;
        }
    }
    CHECK(equal);
}

Word verify_storage(Book& book) {
    const Word events = 2 * book.pairs;
    const Word pages = expected_pages(events);
    const Word spilled = pages * chunk_events;
    const auto bytes = event_bytes();
    const Word page_bytes = (chunk_events / 2) * (bytes.first + bytes.second);
    CHECK(page_bytes <= page_limit);
    const auto& journal = book.core.*access(Journal{});
    if (!CHECK(bool(journal))) return 0; // unsafe prerequisite only
    CHECK(book.core.live_size() == 0 && book.core.history_base() == 0
          && book.core.history_end() == events);
    CHECK(journal->first == spilled && journal->resident_count() == events - spilled
          && journal->resident_count() <= resident_limit);
    CHECK(journal->pages.base() == 0 && journal->pages.size() == pages
          && journal->pages.retained() == pages);
    CHECK(journal->data.size() == pages * page_bytes);
    const Word flushed = flushed_page_rows(pages);
    CHECK(journal->pages.resident() == pages - flushed
          && journal->pages.resident() <= page_tail);
    // JournalPage consists of six uint64 fields. This is the actual logical
    // written extent, not an allocation/preparation size or sizeof of a holder.
    CHECK((journal->pages.*access(PageFile{})).size()
          == flushed * 6 * sizeof(Word));
    bool equal = true;
    Word actual_pages = 0;
    for (Word index = journal->pages.base(); index < journal->pages.size(); ++index) {
        const auto page = journal->pages.get(index);
        const Word start = index * chunk_events;
        const bool matches = page.first_position == start && page.count == chunk_events
            && page.first_ordinal == start + 1 && page.last_ordinal == start + chunk_events
            && page.offset == index * page_bytes && page.length == page_bytes;
        equal = matches && equal;
        ++actual_pages;
    }
    CHECK(equal && actual_pages == pages);
    verify_payload(book, 0, events);
    return journal->data.size();
}

void growth_and_retirement() {
    Book book;
    book.grow_to(1024); // 2048 events: no spill yet.
    CHECK(verify_storage(book) == 0);
    book.grow_to(1025); // Both the journal page and real serialized data exist.
    CHECK(verify_storage(book) > 0);
    book.grow_to(2048);
    const Word small = verify_storage(book); // N commands, 64 pages.
    book.grow_to(8192);
    const Word large = verify_storage(book); // 4N commands, 448 pages.
    CHECK(small > 0 && large == 7 * small);
    // These are Full/Commands-style core operations without the Window
    // consumer's retirement call. No automatic cap on disk is claimed.
    const auto& journal = book.core.*access(Journal{});
    if (!CHECK(bool(journal))) return;
    CHECK(journal->data.size() > 0
          && (journal->pages.*access(PageFile{})).size() > 0
          && journal->data.read_count() > 0 && journal->pages.read_count() > 0);
    // Retiring only half the first page leaves spilled survivors. The source
    // keeps their file and page table until the whole spilled prefix is gone.
    CHECK(book.core.retire_history(16) == 16);
    CHECK(journal->pages.size() == expected_pages(2 * book.pairs)
          && journal->data.size() == large);
    verify_payload(book, 16, 2 * book.pairs);
    // This is the real core retire site called by the Window consumer.
    CHECK(book.core.retire_history(2 * book.pairs) == 2 * book.pairs - 16);
    CHECK(journal->pages.size() == 0 && journal->pages.retained() == 0
          && journal->pages.resident() == 0
          && (journal->pages.*access(PageFile{})).size() == 0);
    CHECK(journal->data.size() == 0 && journal->resident_count() == 0
          && book.core.history().empty());
    bool empty_cache = true;
    for (const auto& slot : journal->cache)
        empty_cache = (!slot.valid && slot.events.empty()) && empty_cache;
    CHECK(empty_cache);
    CHECK(book.core.retire_history(2 * book.pairs) == 0);
}

void reset_without_retirement() {
    Book book;
    book.grow_to(2048);
    CHECK(verify_storage(book) > 0);
    const auto& journal = book.core.*access(Journal{});
    if (!CHECK(bool(journal))) return;
    // Observe lifetime without holding the owning shared_ptr ourselves.
    std::weak_ptr old = journal;
    book.identity = {"journal", 2};
    book.core.reset(book.identity);
    CHECK(old.expired()); // Old page/data owner released with undrained history.
    CHECK(book.core.history_base() == 0 && book.core.history_end() == 0
          && book.core.history().empty());
    if (!CHECK(bool(journal))) return;
    CHECK(journal->pages.size() == 0 && journal->data.size() == 0
          && (journal->pages.*access(PageFile{})).size() == 0);
    book.pairs = 0;
    book.incarnation = book.ordinal = 1;
    book.grow_to(2048);
    CHECK(verify_storage(book) > 0); // Fresh offsets/handles, not an empty-only reset.
}
} // namespace memory_spill_journal

int main() {
    using namespace memory_spill_journal;
    run("command growth, partial and complete retirement", growth_and_retirement);
    run("reset of an undrained spill and fresh run", reset_without_retirement);
    std::printf("memory proof spill_journal: %zu checks, %zu failures\n", checks, failures);
    return failures ? 1 : 0;
}
