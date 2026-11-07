// An oversized native event must survive real spill/reload. The same event
// still obeys an explicitly supplied scratch arena's finite capacity.
// Existing API only: the supervisor can run this TU against the base for RED.
#include <pineforge/native_order.hpp>
#include <pineforge/detail/scratch_arena.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

namespace {
namespace no = pineforge::native_order;
using pineforge::detail::ScratchArena;
using Status = pineforge::detail::ScratchStatus;
constexpr std::size_t kPackingBytes = 32768;
constexpr std::size_t kResidentEvents = 2048;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

std::vector<std::byte> record(const no::CommandEvent& event) {
    const auto source = no::encode_journal_record(event);
    require(source.size() <= std::numeric_limits<std::size_t>::max(),
            "oracle record size is representable");
    std::vector<std::byte> bytes(static_cast<std::size_t>(source.size()));
    require(source.read(0, {bytes.data(), bytes.size()}) == Status::ok,
            "encode every event field for exact comparison");
    return bytes;
}

std::string text(std::size_t size, unsigned salt) {
    std::string result(size, ' ');
    for (std::size_t i = 0; i < size; ++i)
        result[i] = static_cast<char>(' ' + (i * 37 + salt) % 95);
    if (size > 23) result[23] = '\0';
    return result;
}

struct Core {
    no::RunIdentity run{"journal-oversized-event", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    bool direct;

    explicit Core(bool use_direct) : direct(use_direct) {}
    void advance() {
        incarnation = core.last_incarnation() + 1;
        ordinal = core.last_ordinal() + 1;
    }
    no::SubmitResult submit(const no::Request& request, std::int64_t time = 0) {
        advance();
        if (!direct) return core.submit(request, time, incarnation, ordinal);
        no::CommandContext context;
        context.decision_time_ms = time;
        return core.apply_submit(request, context, incarnation, ordinal).result;
    }
    void cancel(const no::RequestHandle& handle) {
        advance();
        const auto result = direct
            ? core.apply_cancel(handle, ordinal).result : core.cancel(handle, ordinal);
        require(result.status == no::CancelStatus::Cancelled, "cancel oversized request");
    }
    void filler() {
        const no::Request refused{no::Transact{0.0}, "filler", "invalid quantity"};
        require(submit(refused).status == no::SubmitStatus::Rejected,
                "one real rejected event per filler");
    }
};

void round_trip(std::size_t label_bytes, bool direct) {
    Core c(direct);
    no::Request request{no::Transact{2.0}, text(label_bytes, 11),
                        text(label_bytes / 3 + 7, 29)};
    request.trigger = no::Limit{51.25};
    const auto result = c.submit(request, 1234567);
    require(result.status == no::SubmitStatus::Accepted && result.handle,
            "oversized request is accepted before spilling");
    std::weak_ptr<const no::RequestDefinition> original =
        std::get<no::AcceptedEvent>(c.core.history_at(0)).definition;
    const auto expected = record(no::AcceptedEvent{
        result.event_ordinal,
        std::make_shared<no::RequestDefinition>(no::RequestDefinition{
            *result.handle, request, {result.event_ordinal, 1234567}, std::nullopt})});
    require(expected.size() > kPackingBytes && request.label.size() > kPackingBytes,
            "actual encoded event and its text alone exceed ordinary packing");
    require(record(c.core.history_at(0)) == expected, "resident acceptance has every input field");
    c.cancel(*result.handle);
    const auto cancelled = record(c.core.history_at(1));
    for (std::size_t i = 0; i < kResidentEvents + 32; ++i) c.filler();

    require(c.core.history_base() == 0 && c.core.live().empty(),
            "retain the complete journal without a live definition owner");
    require(original.expired(), "both definition owners were spilled before any reload");
    require(record(c.core.history_at(0)) == expected, "oversized acceptance reloads exactly");
    require(record(c.core.history_at(1)) == cancelled, "oversized cancellation reloads exactly");
    const auto* found = c.core.event_at({c.run, result.event_ordinal});
    require(found && record(*found) == expected, "ordinal reader finds the complete extent");
}

// The configured backing is real and finite; no injected errno or synthetic
// capacity result substitutes for the arena allocator.
class Arena {
public:
    explicit Arena(std::uint64_t bytes) : file_(std::tmpfile()) {
        require(file_ != nullptr, "create finite arena backing");
        const int fd = ::fileno(file_);
        int error;
#if defined(__linux__)
        do { error = ::posix_fallocate(fd, 0, static_cast<off_t>(bytes)); }
        while (error == EINTR);
#else
        do { error = ::ftruncate(fd, static_cast<off_t>(bytes)); }
        while (error != 0 && errno == EINTR);
#endif
        if (error != 0) {
            std::fclose(file_);
            file_ = nullptr;
            throw std::runtime_error("preallocate finite arena backing");
        }
        try {
            ScratchArena::attach(fd, bytes);
        } catch (...) {
            std::fclose(file_);
            file_ = nullptr;
            throw;
        }
    }
    ~Arena() {
        ScratchArena::detach();
        std::fclose(file_);
    }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
private:
    std::FILE* file_;
};

void finite_provider_refuses_real_extent() {
    constexpr auto chunk = ScratchArena::kChunkBytes;
    Arena arena(2 * chunk);
    Core c(true);
    // One arena chunk for event bytes and one for the page table.
    c.core.reserve(kResidentEvents + 1);
    require(ScratchArena::attached()->stats().bytes_used == 2 * chunk,
            "ordinary preparation fits the explicitly configured provider");
    no::Request large{no::Transact{1.0}, text(static_cast<std::size_t>(chunk) + 257, 17), ""};
    large.trigger = no::Limit{50.0};
    const auto accepted = c.submit(large);
    require(accepted.status == no::SubmitStatus::Accepted && accepted.handle,
            "large resident event precedes provider exhaustion");
    const auto expected = record(c.core.history_at(0));
    std::weak_ptr<const no::RequestDefinition> original =
        std::get<no::AcceptedEvent>(c.core.history_at(0)).definition;
    c.cancel(*accepted.handle);
    while (c.core.history_end() < kResidentEvents) c.filler();
    const auto before_end = c.core.history_end();
    const auto before_ordinal = c.core.last_ordinal();
    const auto before_incarnation = c.core.last_incarnation();
    bool refused = false;
    try {
        c.filler(); // This must append the first event's multi-page extent.
    } catch (const pineforge::detail::ReportScratchIoError& error) {
        const auto failure = pineforge::classify_run_failure(error);
        const std::string wanted =
            "{\"arena_bytes\":" + std::to_string(2 * chunk)
            + ",\"claimed_bytes\":" + std::to_string(2 * chunk)
            + ",\"high_water_bytes\":" + std::to_string(2 * chunk)
            + ",\"needed_bytes_lower_bound\":" + std::to_string(chunk)
            + ",\"reason\":\"arena_exhausted\",\"store\":\"unknown\"}";
        require(error.code().value() == EFBIG
                    && failure.code == pineforge::RunFailureCode::report_sink_failed
                    && failure.args && *failure.args == wanted,
                "real provider exhaustion retains its typed reason and exact demand");
        refused = true;
    }
    require(refused, "a configured finite provider actually refuses the oversized spill");
    require(c.core.history_end() == before_end && c.core.history_base() == 0
                && c.core.last_ordinal() == before_ordinal
                && c.core.last_incarnation() == before_incarnation,
            "provider refusal publishes no event and preserves logical counters");
    require(!original.expired() && record(c.core.history_at(0)) == expected,
            "provider refusal keeps the complete unspilled event");
    require(c.core.retire_history(before_ordinal) == before_end,
            "the retained journal remains readable and retireable after refusal");
    c.filler();
    require(c.core.history_end() == before_end + 1,
            "ordinary commands still work after the reader releases the history");
}
} // namespace

int main() {
    try {
        require(ScratchArena::attached() == nullptr, "test starts without an attached provider");
        for (const std::size_t size : {kPackingBytes + 1, 2 * kPackingBytes + 1,
                                      3 * kPackingBytes + 17,
                                      static_cast<std::size_t>(ScratchArena::kChunkBytes) + 257}) {
            round_trip(size, false);
            round_trip(size, true);
        }
        {
            Arena supplied(8 * ScratchArena::kChunkBytes);
            round_trip(static_cast<std::size_t>(ScratchArena::kChunkBytes) + 257, true);
        }
        finite_provider_refuses_real_extent();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_journal_oversized_event: %s\n", error.what());
        return 1;
    }
    return 0;
}
