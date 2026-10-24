// CR150-CX: the newest book row spills while its acceptance is still resident.
// Static read only in this lane; execution and the bypass mutation are remote.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

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

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                 \
    if (!(condition)) {                                                       \
        ++failures;                                                           \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
    }                                                                         \
} while (false)
#define REQUIRE(condition) do {                                               \
    ++checks;                                                                 \
    if (!(condition)) {                                                       \
        ++failures;                                                           \
        throw std::runtime_error(#condition);                                 \
    }                                                                         \
} while (false)

// make_book_room() spills live_.back() BEFORE appending the next request.
// kSide is a readback lifetime ring, not a deduction from the resident cap.
// No fixture reads a spilled row before its one measured find_live(), so all
// eight ring slots start empty; no earlier recorded owner can be evicted.
constexpr std::uint64_t kBookMax = 4096;
constexpr std::uint64_t kBookSide = 8;
constexpr std::uint64_t kJournalCap = 2048;
constexpr std::uint64_t kJournalChunk = 32;
constexpr std::uint64_t kPopulation = kBookMax + 1;
constexpr std::uint64_t kTarget = kBookMax;
static_assert(kPopulation == 4097 && kTarget == 4096, "first book spill");
static_assert(kJournalCap < kTarget && kBookSide < kJournalCap, "separate tiers");

struct Census {
    std::size_t calls = 0;
    std::size_t definition_calls = 0;
    std::size_t retained_bytes = 0;
};

std::size_t definition_allocation_bytes = 0;
Census* active_census = nullptr;

void record_allocation(std::size_t bytes) noexcept {
    ++active_census->calls;
    if (bytes == definition_allocation_bytes) ++active_census->definition_calls;
}

// Same complete global allocation replacement as X08. Saving the recorder
// state is scalar-only; the measured scope contains no assertions or output.
struct RecordingWindow {
    const bool saved_recording = global_allocation::recording;
    const std::size_t saved_live = global_allocation::live_bytes;
    void (*const saved_callback)(std::size_t) = global_allocation::on_recorded;
    Census* const saved_census = active_census;

    explicit RecordingWindow(Census& census) noexcept {
        active_census = &census;
        global_allocation::live_bytes = 0;
        global_allocation::on_recorded = record_allocation;
        global_allocation::recording = true;
    }
    ~RecordingWindow() {
        global_allocation::recording = false;
        global_allocation::on_recorded = saved_callback;
        global_allocation::live_bytes = saved_live;
        active_census = saved_census;
        global_allocation::recording = saved_recording;
    }
    RecordingWindow(const RecordingWindow&) = delete;
    RecordingWindow& operator=(const RecordingWindow&) = delete;
};

// Standard-library control-block sizes differ. Construct one empty shared
// object of each type independently of WorkingRequestCore, rather than taking
// a byte expectation from another engine run. Both types have empty strings,
// vectors and optional owners here, so this must be exactly one allocation.
template <class T>
std::size_t shared_allocation_bytes() {
    REQUIRE(!global_allocation::recording);
    Census census;
    std::size_t after_free = 0;
    {
        RecordingWindow recording(census);
        {
            const auto owner = std::make_shared<T>();
            census.retained_bytes = global_allocation::live_bytes;
        }
        after_free = global_allocation::live_bytes;
    }
    REQUIRE(census.calls == 1);
    REQUIRE(after_free == 0);
    REQUIRE(census.retained_bytes >= sizeof(T));
    return census.retained_bytes;
}

no::Request limit_request() {
    no::Request request{no::Transact{1.0}, "", ""};
    request.trigger = no::Limit{50.0};
    return request;
}

// A new core per case prevents a previous read's ring owner, a decoded
// journal page, or a marked allocation from contaminating the next census.
void exercise(bool cold, std::size_t live_allocation_bytes) {
    const no::RunIdentity run{"book-read-reuse", cold ? 2u : 1u};
    no::WorkingRequestCore core{run};
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    std::weak_ptr<const no::RequestDefinition> original;
    const no::RequestHandle target{run, kTarget};
    const auto request = limit_request();

    for (std::uint64_t index = 1; index <= kPopulation; ++index) {
        const auto submitted = core.submit(request, 0, incarnation, ordinal);
        REQUIRE(submitted.status == no::SubmitStatus::Accepted && submitted.handle);
        REQUIRE(submitted.handle->incarnation == index);
        REQUIRE(submitted.event_ordinal == index);
        if (index == kTarget) {
            const auto* accepted = std::get_if<no::AcceptedEvent>(&core.history().back());
            REQUIRE(accepted != nullptr && accepted->definition);
            original = accepted->definition; // weak: adds no definition owner
            REQUIRE(original.use_count() == 2); // resident book + acceptance
        }
    }
    REQUIRE(core.live_size() == kPopulation);
    REQUIRE(core.history_base() == 0 && core.history_end() == kPopulation);
    REQUIRE(core.retired_through() == 0);
    REQUIRE(incarnation == kPopulation + 1 && ordinal == kPopulation + 1);
    // Row 4096 is now serialized, so only its resident acceptance owns it.
    REQUIRE(original.use_count() == 1);

    if (cold) {
        // Cancel resident rows 1..2048, never the spilled target. No new
        // request or book read occurs. At end=6145, chunk-rounded first=4128:
        // acceptance position 4095 is cold but still in the unretired window.
        for (std::uint64_t index = 1; index <= kJournalCap; ++index) {
            const auto cancelled = core.cancel(no::RequestHandle{run, index}, ordinal);
            REQUIRE(cancelled.status == no::CancelStatus::Cancelled);
            REQUIRE(cancelled.event_ordinal == kPopulation + index);
        }
        REQUIRE(original.expired());
    }

    const auto events = kPopulation + (cold ? kJournalCap : 0);
    const auto first_resident =
        ((events - kJournalCap + kJournalChunk - 1) / kJournalChunk) * kJournalChunk;
    REQUIRE(first_resident == (cold ? 4128u : 2080u));
    REQUIRE((kTarget - 1 >= first_resident) == !cold);
    REQUIRE(core.history_base() == 0 && core.history_end() == events);
    REQUIRE(core.retired_through() == 0);
    REQUIRE(core.live_size() == kPopulation - (cold ? kJournalCap : 0));

    // Read the resident acceptance before recording, by reference only. The
    // cold case never history_at()s its acceptance: that would decode a page.
    const no::AcceptedEvent* accepted = nullptr;
    if (!cold) {
        accepted = std::get_if<no::AcceptedEvent>(&core.history_at(kTarget - 1));
        REQUIRE(accepted != nullptr && accepted->ordinal == kTarget);
        REQUIRE(accepted->definition && accepted->definition->handle == target);
        REQUIRE(!original.owner_before(accepted->definition));
        REQUIRE(!accepted->definition.owner_before(original));
        REQUIRE(accepted->definition.use_count() == 1);
    }
    REQUIRE(!global_allocation::recording);
    const auto live_before = core.live_size();
    const auto events_before = core.history_end();
    Census census;
    const no::LiveRequest* row = nullptr;
    {
        RecordingWindow recording(census);
        row = core.find_live(target);
        census.retained_bytes = global_allocation::live_bytes;
    }

    // Only find_live() ran under the recorder. The blob byte vector has died;
    // the fresh side-ring slot owns one LiveRequest allocation in both cases.
    REQUIRE(row != nullptr && row->definition);
    CHECK(row->handle() == target && row->birth().acceptance_ordinal == kTarget);
    CHECK(row->priority() == kTarget);
    CHECK(core.live_size() == live_before && core.history_end() == events_before);
    CHECK(core.history_base() == 0 && core.retired_through() == 0);
    if (cold) {
        CHECK(original.expired());
        CHECK(row->definition.use_count() == 1); // only the new ring row
        CHECK(census.definition_calls == 1);
        CHECK(census.retained_bytes == live_allocation_bytes + definition_allocation_bytes);
    } else {
        CHECK(row->definition.get() == accepted->definition.get());
        CHECK(!row->definition.owner_before(accepted->definition));
        CHECK(!accepted->definition.owner_before(row->definition));
        CHECK(row->definition.use_count() == 2); // acceptance + new ring row
        CHECK(original.use_count() == 2);
        // archive_definition() still allocates a temporary definition before
        // the reuse check. Substitution must release it before this read
        // returns: no duplicate owner survives beside the new LiveRequest.
        CHECK(census.retained_bytes == live_allocation_bytes);
    }
    std::printf("book_read_definition_reuse cold=%d population=%llu events=%llu "
                "first_resident=%llu owners=%ld allocation_calls=%zu "
                "definition_calls=%zu retained_bytes=%zu live_owner_bytes=%zu "
                "definition_owner_bytes=%zu\n",
                cold, static_cast<unsigned long long>(kPopulation),
                static_cast<unsigned long long>(events),
                static_cast<unsigned long long>(first_resident),
                row->definition.use_count(), census.calls, census.definition_calls,
                census.retained_bytes, live_allocation_bytes, definition_allocation_bytes);
}
} // namespace

int main() {
    try {
        definition_allocation_bytes = shared_allocation_bytes<no::RequestDefinition>();
        const auto live_allocation_bytes = shared_allocation_bytes<no::LiveRequest>();
        REQUIRE(definition_allocation_bytes != live_allocation_bytes);
#if defined(__GLIBCXX__) && defined(__x86_64__)
        CHECK(definition_allocation_bytes == 456);
#endif
        exercise(false, live_allocation_bytes);
        exercise(true, live_allocation_bytes);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "book_read_definition_reuse: %s\n", error.what());
    }
    std::printf("book_read_definition_reuse checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
