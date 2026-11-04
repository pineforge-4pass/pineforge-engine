// CR150-CX: the newest book row spills while its acceptance is still resident.
// Static read only in this lane; execution and the bypass mutation are remote.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/native_order.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/detail/scratch_errors.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

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

void exercise_held_frame() {
    const no::RunIdentity run{"book-held-frame", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    auto request = limit_request();
    request.label = "held cold definition";
    request.comment = std::string(129, 'q');
    for (std::uint64_t i = 1; i <= kBookMax + kBookSide + 3; ++i) {
        const auto result = core.submit(request, 0, incarnation, ordinal);
        REQUIRE(result.status == no::SubmitStatus::Accepted && result.handle);
    }
    const auto* accepted = std::get_if<no::AcceptedEvent>(&core.history_at(kTarget - 1));
    REQUIRE(accepted && accepted->definition);
    auto held = core.hold_live(no::RequestHandle{run, kTarget});
    REQUIRE(held && held->definition);
    CHECK(held.use_count() == 2); // caller and its side slot
    CHECK(held->definition.get() == accepted->definition.get());
    const auto* address = held.get();
    // Every one of these is another spilled row. Turn over all eight side
    // slots, then read an earlier held object after its decoded buffer died.
    for (std::uint64_t i = 1; i <= kBookSide + 1; ++i) {
        const auto* next = core.find_live(no::RequestHandle{run, kTarget + i});
        REQUIRE(next && next->handle().incarnation == kTarget + i);
    }
    CHECK(held.get() == address && held.use_count() == 1);
    CHECK(held->request().label == request.label);
    CHECK(held->request().comment == request.comment);
    CHECK(held->definition.get() == accepted->definition.get());
    CHECK(held->birth().acceptance_ordinal == kTarget);
}

void exercise_replaced_successor_frame() {
    const no::RunIdentity run{"book-replaced-frame", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    for (std::uint64_t i = 1; i <= kBookMax; ++i) {
        const auto submitted = core.submit(limit_request(), 0, incarnation, ordinal);
        REQUIRE(submitted.status == no::SubmitStatus::Accepted && submitted.handle);
    }
    const no::RequestHandle target{run, kTarget};
    auto successor = limit_request();
    successor.label = "successor definition";
    successor.comment = "same handle, new birth and priority";
    successor.trigger = no::Limit{49.0};
    no::ReplaceOptions options;
    options.keep_handle = true;
    const auto replaced = core.replace(target, successor, 1, incarnation, ordinal,
                                       std::nullopt, options);
    REQUIRE(replaced.status == no::ReplaceStatus::Replaced);
    const auto replacement_position = core.history_end() - 1;
    const auto* resident = std::get_if<no::ReplacedEvent>(&core.history_at(replacement_position));
    REQUIRE(resident && resident->predecessor_definition && resident->successor_definition);
    const auto predecessor = resident->predecessor_definition;
    const auto expected = resident->successor_definition;
    REQUIRE(predecessor->handle == expected->handle);
    REQUIRE(predecessor->birth != expected->birth);
    REQUIRE(predecessor->priority != expected->priority);
    const auto submitted = core.submit(limit_request(), 1, incarnation, ordinal);
    REQUIRE(submitted.status == no::SubmitStatus::Accepted && submitted.handle);
    // The kept-handle successor was newest and is now spilled. Read without
    // promotion; ordinal lookup must choose this ReplacedEvent's successor.
    const auto reads_before = core.book_lookup_reads().rows;
    const auto row = core.hold_live(target);
    REQUIRE(row && row->definition);
    CHECK(core.book_lookup_reads().rows == reads_before + 1);
    CHECK(row->definition.get() == expected.get());
    CHECK(row->definition.get() != predecessor.get());
    CHECK(row->birth() == expected->birth);
    CHECK(row->priority() == expected->priority);
    CHECK(row->request().label == successor.label);
    CHECK(row->request().comment == successor.comment);
    CHECK(std::get<no::Limit>(row->request().trigger).price == 49.0);
}

// Same descriptor seam as the existing spilled-read failure units. Restrict
// the candidates to files opened by the FIRST book spill; never follow names.
std::vector<int> unlinked_descriptors() {
    std::vector<int> found;
    const long limit = ::sysconf(_SC_OPEN_MAX);
    const long end = limit > 0 ? std::min(limit, 4096L) : 1024L;
    for (int fd = 3; fd < end; ++fd) {
        struct stat status{};
        if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_nlink == 0)
            found.push_back(fd);
    }
    return found;
}

int first_frame_descriptor(const std::vector<int>& before) {
    int result = -1;
    for (const int fd : unlinked_descriptors()) {
        if (std::find(before.begin(), before.end(), fd) != before.end()) continue;
        std::array<unsigned char, 10> header{};
        if (::pread(fd, header.data(), header.size(), 0) !=
            static_cast<ssize_t>(header.size())) continue;
        if (header[0] != 1 || header[1] != 1) continue;
        // Independent V1 header recognition, only within the bounded fd delta.
        std::uint64_t decoded = 0;
        for (unsigned i = 0; i != 8; ++i)
            decoded |= std::uint64_t{header[2 + i]} << (8 * i);
        struct stat status{};
        REQUIRE(::fstat(fd, &status) == 0);
        REQUIRE(status.st_size > 10 && decoded > static_cast<std::uint64_t>(status.st_size - 10));
        REQUIRE(result == -1); // exactly one first cold-byte payload file
        result = fd;
    }
    REQUIRE(result != -1);
    return result;
}

class MalformedFrameHost final : public pineforge::NativeStrategyHost {
public:
    explicit MalformedFrameHost(unsigned fault) : fault_(fault) {}
    int frame = -1;
    bool injected = false;
    bool continuation_returned = false;
    bool broker_returned = false;
    bool continuation_failed = false;
    bool broker_failed = false;

    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        if (++bar_ == 1) {
            std::vector<int> before;
            for (std::uint64_t i = 1; i <= kPopulation; ++i) {
                if (i == kPopulation) before = unlinked_descriptors();
                REQUIRE(submit(limit_request()).handle);
            }
            frame = first_frame_descriptor(before);
            return;
        }
        if (bar_ != 2) return;
        REQUIRE(frame >= 0);
        std::array<unsigned char, 10> saved{};
        REQUIRE(::pread(frame, saved.data(), saved.size(), 0) == static_cast<ssize_t>(saved.size()));
        auto damaged = saved;
        if (fault_ == 0) damaged[0] = 2;        // version, no raw fallback
        if (fault_ == 1) damaged[1] = 0xff;     // mode, no raw fallback
        if (fault_ == 2) {
            for (unsigned i = 2; i != 10; ++i) damaged[i] = 0; // expansion exceeds word
        }
        REQUIRE(::pwrite(frame, damaged.data(), damaged.size(), 0) ==
                static_cast<ssize_t>(damaged.size()));
        injected = true;
        try {
            (void)native_continuation_hash();
            continuation_returned = true;
        } catch (const pineforge::detail::AuxiliaryScratchIoError& error) {
            continuation_failed = error.code().value() == EIO
                && error.run_failure_code() == pineforge::RunFailureCode::scratch_io_failed;
        }
        // Repair physical bytes; a carried first failure must still prevent
        // the next logical reader from answering a partial/default hash.
        REQUIRE(::pwrite(frame, saved.data(), saved.size(), 0) == static_cast<ssize_t>(saved.size()));
        try {
            (void)broker_state_hash();
            broker_returned = true;
        } catch (const pineforge::detail::AuxiliaryScratchIoError& error) {
            broker_failed = error.code().value() == EIO
                && error.run_failure_code() == pineforge::RunFailureCode::scratch_io_failed;
        }
    }
private:
    unsigned fault_;
    int bar_ = 0;
};

void exercise_failed_frames() {
    pineforge::NativeRunSpec spec;
    spec.identity = {"book-framed-failure", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:COLD";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = pineforge::NativeSlotLabelPolicy::FeedTolerant;
    spec.report_policy = pineforge::NativeReportPolicy::HostRecorded;
    spec.initial_capital = 1.0e9;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    std::array<pineforge::Bar, 3> bars{};
    for (std::size_t i = 0; i != bars.size(); ++i) {
        auto& bar = bars[i];
        bar.open = bar.high = bar.low = bar.close = 100;
        bar.volume = 1;
        bar.timestamp = 1736121600000LL + static_cast<std::int64_t>(i) * 300000;
    }
    for (unsigned fault = 0; fault != 3; ++fault) {
        MalformedFrameHost host{fault};
        REQUIRE(host.configure_native(spec).status == pineforge::NativeSetupStatus::Applied);
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.injected);
        CHECK(host.continuation_failed && !host.continuation_returned);
        CHECK(host.broker_failed && !host.broker_returned);
        CHECK(host.native_state().kind == pineforge::NativeLifecycleKind::Failed);
        CHECK(pineforge::run_failure_value_of(host).code == pineforge::RunFailureCode::scratch_io_failed);
    }
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
        exercise_held_frame();
        exercise_replaced_successor_frame();
        exercise_failed_frames();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "book_read_definition_reuse: %s\n", error.what());
    }
    std::printf("book_read_definition_reuse checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
